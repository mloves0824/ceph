// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <boost/bind.hpp>

#include "common/Formatter.h"
#include "common/admin_socket.h"
#include "common/ceph_argparse.h"
#include "common/code_environment.h"
#include "common/common_init.h"
#include "common/debug.h"
#include "common/errno.h"
#include "include/stringify.h"
#include "cls/rbd/cls_rbd_client.h"
#include "librbd/ObjectWatcher.h"
#include "librbd/internal.h"
#include "Replayer.h"
#include "Threads.h"

#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd-mirror: Replayer::" << __func__ << ": "

using std::chrono::seconds;
using std::map;
using std::string;
using std::unique_ptr;
using std::vector;

using librbd::cls_client::dir_get_name;

namespace rbd {
namespace mirror {

namespace {

class ReplayerAdminSocketCommand {
public:
  virtual ~ReplayerAdminSocketCommand() {}
  virtual bool call(Formatter *f, stringstream *ss) = 0;
};

class StatusCommand : public ReplayerAdminSocketCommand {
public:
  explicit StatusCommand(Replayer *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    replayer->print_status(f, ss);
    return true;
  }

private:
  Replayer *replayer;
};

class StartCommand : public ReplayerAdminSocketCommand {
public:
  explicit StartCommand(Replayer *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    replayer->start();
    return true;
  }

private:
  Replayer *replayer;
};

class StopCommand : public ReplayerAdminSocketCommand {
public:
  explicit StopCommand(Replayer *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    replayer->stop();
    return true;
  }

private:
  Replayer *replayer;
};

class RestartCommand : public ReplayerAdminSocketCommand {
public:
  explicit RestartCommand(Replayer *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    replayer->restart();
    return true;
  }

private:
  Replayer *replayer;
};

class FlushCommand : public ReplayerAdminSocketCommand {
public:
  explicit FlushCommand(Replayer *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    replayer->flush();
    return true;
  }

private:
  Replayer *replayer;
};

} // anonymous namespace

class ReplayerAdminSocketHook : public AdminSocketHook {
public:
  ReplayerAdminSocketHook(CephContext *cct, const std::string &name,
			  Replayer *replayer) :
    admin_socket(cct->get_admin_socket()) {
    std::string command;
    int r;

    command = "rbd mirror status " + name;
    r = admin_socket->register_command(command, command, this,
				       "get status for rbd mirror " + name);
    if (r == 0) {
      commands[command] = new StatusCommand(replayer);
    }

    command = "rbd mirror start " + name;
    r = admin_socket->register_command(command, command, this,
				       "start rbd mirror " + name);
    if (r == 0) {
      commands[command] = new StartCommand(replayer);
    }

    command = "rbd mirror stop " + name;
    r = admin_socket->register_command(command, command, this,
				       "stop rbd mirror " + name);
    if (r == 0) {
      commands[command] = new StopCommand(replayer);
    }

    command = "rbd mirror restart " + name;
    r = admin_socket->register_command(command, command, this,
				       "restart rbd mirror " + name);
    if (r == 0) {
      commands[command] = new RestartCommand(replayer);
    }

    command = "rbd mirror flush " + name;
    r = admin_socket->register_command(command, command, this,
				       "flush rbd mirror " + name);
    if (r == 0) {
      commands[command] = new FlushCommand(replayer);
    }
  }

  ~ReplayerAdminSocketHook() {
    for (Commands::const_iterator i = commands.begin(); i != commands.end();
	 ++i) {
      (void)admin_socket->unregister_command(i->first);
      delete i->second;
    }
  }

  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) {
    Commands::const_iterator i = commands.find(command);
    assert(i != commands.end());
    Formatter *f = Formatter::create(format);
    stringstream ss;
    bool r = i->second->call(f, &ss);
    delete f;
    out.append(ss);
    return r;
  }

private:
  typedef std::map<std::string, ReplayerAdminSocketCommand*> Commands;

  AdminSocket *admin_socket;
  Commands commands;
};

class MirrorStatusWatchCtx {
public:
  MirrorStatusWatchCtx(librados::IoCtx &ioctx, ContextWQ *work_queue) {
    m_ioctx.dup(ioctx);
    m_watcher = new Watcher(m_ioctx, work_queue);
  }

  ~MirrorStatusWatchCtx() {
    delete m_watcher;
  }

  int register_watch() {
    C_SaferCond cond;
    m_watcher->register_watch(&cond);
    return cond.wait();
  }

  int unregister_watch() {
    C_SaferCond cond;
    m_watcher->unregister_watch(&cond);
    return cond.wait();
  }

  std::string get_oid() const {
    return m_watcher->get_oid();
  }

private:
  class Watcher : public librbd::ObjectWatcher<> {
  public:
    Watcher(librados::IoCtx &ioctx, ContextWQ *work_queue) :
      ObjectWatcher<>(ioctx, work_queue) {
    }

    virtual std::string get_oid() const {
      return RBD_MIRRORING;
    }

    virtual void handle_notify(uint64_t notify_id, uint64_t handle,
			       bufferlist &bl) {
      bufferlist out;
      acknowledge_notify(notify_id, handle, out);
    }
  };

  librados::IoCtx m_ioctx;
  Watcher *m_watcher;
};

Replayer::Replayer(Threads *threads, std::shared_ptr<ImageDeleter> image_deleter,
                   RadosRef local_cluster, const peer_t &peer,
                   const std::vector<const char*> &args) :
  m_threads(threads),
  m_image_deleter(image_deleter),
  m_lock(stringify("rbd::mirror::Replayer ") + stringify(peer)),
  m_peer(peer),
  m_args(args),
  m_local(local_cluster),
  m_remote(new librados::Rados),
  m_asok_hook(nullptr),
  m_replayer_thread(this)
{
  CephContext *cct = static_cast<CephContext *>(m_local->cct());
  m_asok_hook = new ReplayerAdminSocketHook(cct, m_peer.cluster_name, this);
}

Replayer::~Replayer()
{
  delete m_asok_hook;

  m_stopping.set(1);
  {
    Mutex::Locker l(m_lock);
    m_cond.Signal();
  }
  if (m_replayer_thread.is_started()) {
    m_replayer_thread.join();
  }
}

int Replayer::init()
{
  dout(20) << "replaying for " << m_peer << dendl;

  // NOTE: manually bootstrap a CephContext here instead of via
  // the librados API to avoid mixing global singletons between
  // the librados shared library and the daemon
  // TODO: eliminate intermingling of global singletons within Ceph APIs
  CephInitParameters iparams(CEPH_ENTITY_TYPE_CLIENT);
  if (m_peer.client_name.empty() ||
      !iparams.name.from_str(m_peer.client_name)) {
    derr << "error initializing remote cluster handle for " << m_peer << dendl;
    return -EINVAL;
  }

  CephContext *cct = common_preinit(iparams, CODE_ENVIRONMENT_LIBRARY,
                                    CINIT_FLAG_UNPRIVILEGED_DAEMON_DEFAULTS);
  cct->_conf->cluster = m_peer.cluster_name;

  // librados::Rados::conf_read_file
  int r = cct->_conf->parse_config_files(nullptr, nullptr, 0);
  if (r < 0) {
    derr << "could not read ceph conf for " << m_peer << ": "
	 << cpp_strerror(r) << dendl;
    cct->put();
    return r;
  }
  cct->_conf->parse_env();

  // librados::Rados::conf_parse_env
  std::vector<const char*> args;
  env_to_vec(args, nullptr);
  r = cct->_conf->parse_argv(args);
  if (r < 0) {
    derr << "could not parse environment for " << m_peer << ":"
         << cpp_strerror(r) << dendl;
    cct->put();
    return r;
  }

  if (!m_args.empty()) {
    // librados::Rados::conf_parse_argv
    r = cct->_conf->parse_argv(m_args);
    if (r < 0) {
      derr << "could not parse command line args for " << m_peer << ": "
	   << cpp_strerror(r) << dendl;
      cct->put();
      return r;
    }
  }

  // disable unnecessary librbd cache
  cct->_conf->set_val_or_die("rbd_cache", "false");
  cct->_conf->apply_changes(nullptr);
  cct->_conf->complain_about_parse_errors(cct);

  r = m_remote->init_with_context(cct);
  assert(r == 0);
  cct->put();

  r = m_remote->connect();
  if (r < 0) {
    derr << "error connecting to remote cluster " << m_peer
	 << " : " << cpp_strerror(r) << dendl;
    return r;
  }

  dout(20) << "connected to " << m_peer << dendl;

  // Bootstrap existing mirroring images
  init_local_mirroring_images();

  // TODO: make interval configurable
  m_pool_watcher.reset(new PoolWatcher(m_remote, 30, m_lock, m_cond));
  m_pool_watcher->refresh_images();

  m_replayer_thread.create("replayer");

  return 0;
}

void Replayer::init_local_mirroring_images() {
  list<pair<int64_t, string> > pools;
  int r = m_local->pool_list2(pools);
  if (r < 0) {
    derr << "error listing pools: " << cpp_strerror(r) << dendl;
    return;
  }

  for (auto kv : pools) {
    int64_t pool_id = kv.first;
    string pool_name = kv.second;
    int64_t base_tier;
    r = m_local->pool_get_base_tier(pool_id, &base_tier);
    if (r == -ENOENT) {
      dout(10) << "pool " << pool_name << " no longer exists" << dendl;
      continue;
    } else if (r < 0) {
      derr << "Error retrieving base tier for pool " << pool_name << dendl;
      continue;
    }
    if (pool_id != base_tier) {
      // pool is a cache; skip it
      continue;
    }

    librados::IoCtx ioctx;
    r = m_local->ioctx_create2(pool_id, ioctx);
    if (r == -ENOENT) {
      dout(10) << "pool " << pool_name << " no longer exists" << dendl;
      continue;
    } else if (r < 0) {
      derr << "Error accessing pool " << pool_name << cpp_strerror(r) << dendl;
      continue;
    }

    rbd_mirror_mode_t mirror_mode;
    r = librbd::mirror_mode_get(ioctx, &mirror_mode);
    if (r < 0) {
      derr << "could not tell whether mirroring was enabled for " << pool_name
	   << " : " << cpp_strerror(r) << dendl;
      continue;
    }
    if (mirror_mode == RBD_MIRROR_MODE_DISABLED) {
      dout(20) << "pool " << pool_name << " has mirroring disabled" << dendl;
      continue;
    }

    librados::IoCtx remote_ioctx;
    r = m_remote->ioctx_create(ioctx.get_pool_name().c_str(), remote_ioctx);
    if (r < 0 && r != -ENOENT) {
      dout(10) << "Error connecting to remote pool " << ioctx.get_pool_name()
               << ": " << cpp_strerror(r) << dendl;
      continue;
    } else if (r == -ENOENT) {
      // remote pool does not exist anymore, we are going to add the images
      // with local pool id
      pool_id = ioctx.get_id();
    }
    else {
      pool_id = remote_ioctx.get_id();
    }

    std::set<InitImageInfo> images;

    std::string last_read = "";
    int max_read = 1024;
    do {
      std::map<std::string, std::string> mirror_images;
      r = librbd::cls_client::mirror_image_list(&ioctx, last_read, max_read,
                                                &mirror_images);
      if (r < 0) {
        derr << "error listing mirrored image directory: "
             << cpp_strerror(r) << dendl;
        continue;
      }
      for (auto it = mirror_images.begin(); it != mirror_images.end(); ++it) {
        std::string image_name;
        r = dir_get_name(&ioctx, RBD_DIRECTORY, it->first, &image_name);
        if (r < 0) {
          derr << "error retrieving local image name: " << cpp_strerror(r)
               << dendl;
          continue;
        }
        images.insert(InitImageInfo(it->second, ioctx.get_id(), it->first,
                                    image_name));
      }
      if (!mirror_images.empty()) {
        last_read = mirror_images.rbegin()->first;
      }
      r = mirror_images.size();
    } while (r == max_read);

    if (!images.empty()) {
      m_init_images[pool_id] = std::move(images);
    }
  }
}

void Replayer::run()
{
  dout(20) << "enter" << dendl;

  while (!m_stopping.read()) {
    Mutex::Locker l(m_lock);
    if (!m_manual_stop) {
      set_sources(m_pool_watcher->get_images());
    }
    m_cond.WaitInterval(g_ceph_context, m_lock, seconds(30));
  }

  m_image_deleter.reset();

  PoolImageIds empty_sources;
  while (true) {
    Mutex::Locker l(m_lock);
    set_sources(empty_sources);
    if (m_images.empty()) {
      break;
    }
    m_cond.WaitInterval(g_ceph_context, m_lock, seconds(1));
  }
}

void Replayer::print_status(Formatter *f, stringstream *ss)
{
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_lock);

  if (f) {
    f->open_object_section("replayer_status");
    f->dump_stream("peer") << m_peer;
    f->open_array_section("image_replayers");
  };

  for (auto it = m_images.begin(); it != m_images.end(); it++) {
    auto &pool_images = it->second;
    for (auto i = pool_images.begin(); i != pool_images.end(); i++) {
      auto &image_replayer = i->second;
      image_replayer->print_status(f, ss);
    }
  }

  if (f) {
    f->close_section();
    f->close_section();
    f->flush(*ss);
  }
}

void Replayer::start()
{
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_lock);

  if (m_stopping.read()) {
    return;
  }

  m_manual_stop = false;

  for (auto it = m_images.begin(); it != m_images.end(); it++) {
    auto &pool_images = it->second;
    for (auto i = pool_images.begin(); i != pool_images.end(); i++) {
      auto &image_replayer = i->second;
      image_replayer->start(nullptr, nullptr, true);
    }
  }
}

void Replayer::stop()
{
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_lock);

  if (m_stopping.read()) {
    return;
  }

  m_manual_stop = true;

  for (auto it = m_images.begin(); it != m_images.end(); it++) {
    auto &pool_images = it->second;
    for (auto i = pool_images.begin(); i != pool_images.end(); i++) {
      auto &image_replayer = i->second;
      image_replayer->stop(nullptr, true);
    }
  }
}

void Replayer::restart()
{
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_lock);

  if (m_stopping.read()) {
    return;
  }

  m_manual_stop = false;

  for (auto it = m_images.begin(); it != m_images.end(); it++) {
    auto &pool_images = it->second;
    for (auto i = pool_images.begin(); i != pool_images.end(); i++) {
      auto &image_replayer = i->second;
      image_replayer->restart();
    }
  }
}

void Replayer::flush()
{
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_lock);

  if (m_stopping.read() || m_manual_stop) {
    return;
  }

  for (auto it = m_images.begin(); it != m_images.end(); it++) {
    auto &pool_images = it->second;
    for (auto i = pool_images.begin(); i != pool_images.end(); i++) {
      auto &image_replayer = i->second;
      image_replayer->flush();
    }
  }
}

void Replayer::set_sources(const PoolImageIds &pool_image_ids)
{
  dout(20) << "enter" << dendl;

  assert(m_lock.is_locked());

  if (!m_init_images.empty()) {
    dout(20) << "m_init_images has images!" << dendl;
    for (auto it = m_init_images.begin(); it != m_init_images.end(); ++it) {
      int64_t pool_id = it->first;
      std::set<InitImageInfo>& images = it->second;
      auto remote_pool_it = pool_image_ids.find(pool_id);
      if (remote_pool_it != pool_image_ids.end()) {
        const std::set<ImageIds>& remote_images = remote_pool_it->second;
        for (const auto& remote_image : remote_images) {
          auto image = images.find(InitImageInfo(remote_image.global_id));
          if (image != images.end()) {
            images.erase(image);
          }
        }
      }
    }
    // the remaining images in m_init_images must be deleted
    for (auto it = m_init_images.begin(); it != m_init_images.end(); ++it) {
      for (const auto& image : it->second) {
        dout(20) << "scheduling the deletion of init image: "
                 << image.name << dendl;
        m_image_deleter->schedule_image_delete(image.pool_id, image.id,
                                               image.name, image.global_id);
      }
    }
    m_init_images.clear();
  } else {
    dout(20) << "m_init_images is empty!" << dendl;
  }

  for (auto it = m_images.begin(); it != m_images.end();) {
    int64_t pool_id = it->first;
    auto &pool_images = it->second;

    // pool has no mirrored images
    if (pool_image_ids.find(pool_id) == pool_image_ids.end()) {
      dout(20) << "pool " << pool_id << " has no mirrored images" << dendl;
      for (auto images_it = pool_images.begin();
	   images_it != pool_images.end();) {
        if (images_it->second->is_running()) {
          dout(20) << "stop image replayer for "
                   << images_it->second->get_global_image_id() << dendl;
        }
	if (stop_image_replayer(images_it->second)) {
	  images_it = pool_images.erase(images_it);
	} else {
          ++images_it;
        }
      }
      if (pool_images.empty()) {
	mirror_image_status_shut_down(pool_id);
	it = m_images.erase(it);
      } else {
        ++it;
      }
      continue;
    }

    // shut down replayers for non-mirrored images
    for (auto images_it = pool_images.begin();
	 images_it != pool_images.end();) {
      auto &image_ids = pool_image_ids.at(pool_id);
      if (image_ids.find(ImageIds(images_it->first)) == image_ids.end()) {
        if (images_it->second->is_running()) {
          dout(20) << "stop image replayer for "
                   << images_it->second->get_global_image_id() << dendl;
        }
	if (stop_image_replayer(images_it->second)) {
	  images_it = pool_images.erase(images_it);
	} else {
	  ++images_it;
	}
      } else {
	++images_it;
      }
    }
    ++it;
  }

  // (re)start new image replayers
  for (const auto &kv : pool_image_ids) {
    int64_t pool_id = kv.first;

    // TODO: clean up once remote peer -> image replayer refactored
    librados::IoCtx remote_ioctx;
    int r = m_remote->ioctx_create2(pool_id, remote_ioctx);
    if (r < 0) {
      derr << "failed to lookup remote pool " << pool_id << ": "
           << cpp_strerror(r) << dendl;
      continue;
    }

    librados::IoCtx local_ioctx;
    r = m_local->ioctx_create(remote_ioctx.get_pool_name().c_str(), local_ioctx);
    if (r < 0) {
      derr << "failed to lookup local pool " << remote_ioctx.get_pool_name()
           << ": " << cpp_strerror(r) << dendl;
      continue;
    }

    std::string local_mirror_uuid;
    r = librbd::cls_client::mirror_uuid_get(&local_ioctx, &local_mirror_uuid);
    if (r < 0) {
      derr << "failed to retrieve local mirror uuid from pool "
        << local_ioctx.get_pool_name() << ": " << cpp_strerror(r) << dendl;
      continue;
    }

    std::string remote_mirror_uuid;
    r = librbd::cls_client::mirror_uuid_get(&remote_ioctx, &remote_mirror_uuid);
    if (r < 0) {
      derr << "failed to retrieve remote mirror uuid from pool "
        << remote_ioctx.get_pool_name() << ": " << cpp_strerror(r) << dendl;
      continue;
    }

    // create entry for pool if it doesn't exist
    auto &pool_replayers = m_images[pool_id];

    if (pool_replayers.empty()) {
      r = mirror_image_status_init(pool_id, local_ioctx);
      if (r < 0) {
	continue;
      }
    }

    for (const auto &image_id : kv.second) {
      auto it = pool_replayers.find(image_id.id);
      if (it == pool_replayers.end()) {
	unique_ptr<ImageReplayer<> > image_replayer(new ImageReplayer<>(
          m_threads, m_local, m_remote, local_mirror_uuid, remote_mirror_uuid,
          local_ioctx.get_id(), pool_id, image_id.id, image_id.global_id));
	it = pool_replayers.insert(
	  std::make_pair(image_id.id, std::move(image_replayer))).first;
      }
      if (!it->second->is_running()) {
        dout(20) << "starting image replayer for "
                 << it->second->get_global_image_id() << dendl;
      }
      start_image_replayer(it->second, image_id.name);
    }
  }
}

int Replayer::mirror_image_status_init(int64_t pool_id,
				       librados::IoCtx& ioctx) {
  assert(m_status_watchers.find(pool_id) == m_status_watchers.end());

  uint64_t instance_id = librados::Rados(ioctx).get_instance_id();

  dout(20) << "pool_id=" << pool_id << ", instance_id=" << instance_id << dendl;

  librados::ObjectWriteOperation op;
  librbd::cls_client::mirror_image_status_remove_down(&op);
  int r = ioctx.operate(RBD_MIRRORING, &op);
  if (r < 0) {
    derr << "error initializing " << RBD_MIRRORING << "object: "
	 << cpp_strerror(r) << dendl;
    return r;
  }

  unique_ptr<MirrorStatusWatchCtx>
    watch_ctx(new MirrorStatusWatchCtx(ioctx, m_threads->work_queue));

  r = watch_ctx->register_watch();
  if (r < 0) {
    derr << "error registering watcher for " << watch_ctx->get_oid()
	 << " object: " << cpp_strerror(r) << dendl;
    return r;
  }

  m_status_watchers.insert(std::make_pair(pool_id, std::move(watch_ctx)));

  return 0;
}

void Replayer::mirror_image_status_shut_down(int64_t pool_id) {
  auto watcher_it = m_status_watchers.find(pool_id);
  assert(watcher_it != m_status_watchers.end());

  int r = watcher_it->second->unregister_watch();
  if (r < 0) {
    derr << "error unregistering watcher for " << watcher_it->second->get_oid()
	 << " object: " << cpp_strerror(r) << dendl;
  }

  m_status_watchers.erase(watcher_it);
}

void Replayer::start_image_replayer(unique_ptr<ImageReplayer<> > &image_replayer,
                                    const boost::optional<std::string>& image_name)
{
  if (!image_replayer->is_stopped()) {
    return;
  }

  if (image_name) {
    FunctionContext *ctx = new FunctionContext(
        [&] (int r) {
          if (r >= 0) {
            image_replayer->start();
          } else {
            start_image_replayer(image_replayer, image_name);
          }
       }
    );
    m_image_deleter->wait_for_scheduled_deletion(image_name.get(), ctx, false);
  }
}

bool Replayer::stop_image_replayer(unique_ptr<ImageReplayer<> > &image_replayer)
{
  if (image_replayer->is_stopped()) {
    return true;
  }

  if (image_replayer->is_running()) {
    FunctionContext *ctx = new FunctionContext(
        [&image_replayer, this] (int r) {
          if (m_image_deleter) {
            m_image_deleter->schedule_image_delete(
                          image_replayer->get_local_pool_id(),
                          image_replayer->get_local_image_id(),
                          image_replayer->get_local_image_name(),
                          image_replayer->get_global_image_id());
          }
        }
    );
    image_replayer->stop(ctx);
  } else {
    // TODO: checkhow long it is stopping and alert if it is too long.
  }

  return false;
}

} // namespace mirror
} // namespace rbd