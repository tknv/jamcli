#include "../include/video.h"
#include "../include/utils.h"

#include <chrono>
#include <cstdio>

#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

// ============================================================================
// PeerVideoRenderer
// ============================================================================
PeerVideoRenderer::~PeerVideoRenderer() { stopBlocking(); }

void PeerVideoRenderer::setDebugCallback(std::function<void(std::string)> cb) {
  debug_cb_ = std::move(cb);
}

bool PeerVideoRenderer::start(const std::string &sinkId, int width, int height,
                              const std::string &peerLabel,
                              const std::string &role,
                              const std::string &rendererMode, int listenFd) {
  requestStop();
  last_error_.clear();

  if (rendererMode == "raw" && listenFd < 0) {
    last_error_ = "no renderer socket is available for this call";
    return false;
  }

  width_ = width;
  height_ = height;
  sink_id_ = sinkId;
  role_ = role;
  renderer_mode_ = rendererMode;
  listen_fd_ = listenFd;
  peer_label_ = peerLabel;
  frames_received_ = 0;

  writer_running_ = true;
  writer_thread_ = std::thread(&PeerVideoRenderer::writerLoop, this);

  if (!attachSinkTarget()) {
    requestStop();
    return false;
  }
  registered_ = true;
  paused_ = false;

  watchdog_running_ = true;
  watchdog_thread_ = std::thread([this] {
    for (int i = 0; i < 30 && watchdog_running_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (watchdog_running_ && frames_received_ == 0 && debug_cb_) {
      debug_cb_("no video frames received from the daemon after 3s");
    }
  });

  return true;
}

const std::string &PeerVideoRenderer::lastError() const { return last_error_; }

void PeerVideoRenderer::pause() {
  paused_ = true;
  if (debug_cb_)
    debug_cb_("image data transfer stopped for sink '" + sink_id_ + "'");
}

void PeerVideoRenderer::resume() {
  if (!registered_)
    return;
  paused_ = false;
  if (debug_cb_)
    debug_cb_("resumed for sink '" + sink_id_ + "'");
}

bool PeerVideoRenderer::isPaused() const { return paused_; }

void PeerVideoRenderer::requestStop() {
  watchdog_running_ = false;
  if (watchdog_thread_.joinable())
    watchdog_thread_.join();

  bool wasRegistered = registered_.exchange(false);
  paused_ = false;
  writer_running_ = false;
  queue_cv_.notify_all();

  closeConnection();

  if (writer_thread_.joinable())
    writer_thread_.join();

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending_frame_.reset();
  }

  listen_fd_ = -1;

  if (!wasRegistered)
    return;

  std::string sinkId = std::move(sink_id_);
  int frames = frames_received_.load();

  // Synchronous on purpose: start() calls requestStop() and then
  // immediately re-registers this same sink id when the daemon
  // restarts decoding for it (e.g. the peer's video resolution or
  // orientation changed mid-call). Doing the unregister on a
  // detached thread raced with that re-registration and could wipe
  // out the freshly attached target, which is why the remote view
  // would go dark ("no video frames received") right after a resize.
  libjami::registerSinkTarget(sinkId, libjami::SinkTarget{});
  if (debug_cb_) {
    debug_cb_("stopping (received " + std::to_string(frames) +
              " frame(s) total for sink '" + sinkId + "')");
  }
}

void PeerVideoRenderer::stopBlocking() {
  watchdog_running_ = false;
  if (watchdog_thread_.joinable())
    watchdog_thread_.join();
  writer_running_ = false;
  queue_cv_.notify_all();
  closeConnection();
  if (writer_thread_.joinable())
    writer_thread_.join();
  if (registered_.exchange(false)) {
    libjami::registerSinkTarget(sink_id_, libjami::SinkTarget{});
    if (debug_cb_) {
      debug_cb_("stopping (received " +
                std::to_string(frames_received_.load()) +
                " frame(s) total for sink '" + sink_id_ + "')");
    }
  }
}

bool PeerVideoRenderer::attachSinkTarget() {
  libjami::SinkTarget target;
  target.preferredFormat = AV_PIX_FMT_RGB0;
  target.pull = [this]() -> libjami::FrameBuffer { return allocFrame(); };
  target.push = [this](libjami::FrameBuffer buf) {
    enqueueFrame(std::move(buf));
  };
  if (!libjami::registerSinkTarget(sink_id_, target)) {
    last_error_ =
        "registerSinkTarget() found no sink for id '" + sink_id_ + "'";
    return false;
  }
  return true;
}

libjami::FrameBuffer PeerVideoRenderer::allocFrame() {
  AVFrame *f = av_frame_alloc();
  if (!f)
    return nullptr;
  f->format = AV_PIX_FMT_RGB0;
  f->width = width_;
  f->height = height_;
  if (av_frame_get_buffer(f, 32) < 0) {
    av_frame_free(&f);
    return nullptr;
  }
  return libjami::FrameBuffer(f);
}

void PeerVideoRenderer::enqueueFrame(libjami::FrameBuffer buf) {
  if (paused_)
    return;
  int prev = frames_received_.fetch_add(1);
  if (prev == 0 && debug_cb_) {
    debug_cb_("first video frame received from the daemon");
  }
  std::lock_guard<std::mutex> lock(queue_mutex_);
  pending_frame_ = std::move(buf);
  queue_cv_.notify_one();
}

void PeerVideoRenderer::writerLoop() {
  if (renderer_mode_ == "raw") {
    writerLoopRawSocket();
  } else {
    writerLoopPlayerPipe();
  }
}

void PeerVideoRenderer::writerLoopRawSocket() {
  int fd = -1;
  while (writer_running_ && fd < 0) {
    pollfd pfd{listen_fd_, POLLIN, 0};
    int rc = ::poll(&pfd, 1, 100);
    if (rc > 0 && (pfd.revents & POLLIN)) {
      fd = ::accept(listen_fd_, nullptr, nullptr);
    }
  }
  if (fd < 0)
    return;
  conn_fd_ = fd;
  if (debug_cb_)
    debug_cb_("--raw-renderer client connected for sink '" + sink_id_ + "'");

  while (writer_running_) {
    libjami::FrameBuffer frame;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] {
        return pending_frame_ != nullptr || !writer_running_;
      });
      if (!writer_running_)
        break;
      frame = std::move(pending_frame_);
    }
    if (frame && !writeFrameToSocket(frame.get()))
      break;
  }

  closeConnection();
}

void PeerVideoRenderer::writerLoopPlayerPipe() {
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    if (debug_cb_)
      debug_cb_("pipe() failed - cannot start " + renderer_mode_);
    return;
  }
  pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(pipefd[0], STDIN_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    std::string logPath =
        "/tmp/jamcli-renderer-" + std::to_string(::getpid()) + ".log";
    std::freopen(logPath.c_str(), "w", stdout);
    std::freopen(logPath.c_str(), "a", stderr);
    std::string videoSize =
        std::to_string(width_) + "x" + std::to_string(height_);
    std::string title = (role_ == "local") ? "Local View" : "Remote View";
    if (renderer_mode_ == "mpv") {
      std::string wArg = "--demuxer-rawvideo-w=" + std::to_string(width_);
      std::string hArg = "--demuxer-rawvideo-h=" + std::to_string(height_);
      std::string titleArg = "--title=" + title;
      const char *flipFilter = (role_ == "local") ? "hflip" : "null";
      std::string vfArg = std::string("--vf=") + flipFilter;
      ::execlp("mpv", "mpv", "--demuxer=rawvideo", wArg.c_str(), hArg.c_str(),
               "--demuxer-rawvideo-mp-format=rgb0", vfArg.c_str(), titleArg.c_str(),
               "--really-quiet", "-", static_cast<char *>(nullptr));
    } else {
      const char *flipFilter = (role_ == "local") ? "hflip" : "null";
      ::execlp("ffplay", "ffplay", "-hide_banner", "-loglevel", "error", "-f",
               "rawvideo", "-pixel_format", "rgb0", "-video_size",
               videoSize.c_str(), "-framerate",
               "30", // ★パイプ入力ではFPS指定が必須
               "-window_title", title.c_str(),
               "-infbuf", // リアルタイムストリーム用のバッファ設定
               "-vf", flipFilter, "-autoexit", "-i", "pipe:0",
               static_cast<char *>(nullptr));
    }
    std::fprintf(stderr, "jamcli: exec of '%s' failed\n",
                 renderer_mode_.c_str());
    ::_exit(127);
  }
  ::close(pipefd[0]);
  if (pid < 0) {
    if (debug_cb_)
      debug_cb_("fork() failed - cannot start " + renderer_mode_);
    ::close(pipefd[1]);
    return;
  }
  player_pid_ = pid;
  if (debug_cb_) {
    debug_cb_(renderer_mode_ + " started (pid " + std::to_string(pid) +
              ") for sink '" + sink_id_ + "'");
  }

  while (writer_running_) {
    libjami::FrameBuffer frame;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] {
        return pending_frame_ != nullptr || !writer_running_;
      });
      if (!writer_running_)
        break;
      frame = std::move(pending_frame_);
    }
    if (frame && !writePixelsToPipe(pipefd[1], frame.get()))
      break;
  }

  ::close(pipefd[1]);
  for (int i = 0; i < 30; ++i) {
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) > 0) {
      pid = -1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (pid > 0) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
  }
  player_pid_ = -1;
}

bool PeerVideoRenderer::writePixelsToPipe(int fd, AVFrame *f) {
  if (!f || fd < 0)
    return false;
  const size_t rowBytes = static_cast<size_t>(width_) * 4;
  for (int y = 0; y < height_; ++y) {
    const uint8_t *row = f->data[0] + static_cast<size_t>(y) * f->linesize[0];
    if (!writeAll(fd, row, rowBytes))
      return false;
  }
  return true;
}

bool PeerVideoRenderer::writeFrameToSocket(AVFrame *f) {
  int fd = conn_fd_.load();
  if (!f || fd < 0)
    return false;
  uint32_t payloadLen =
      8 + static_cast<uint32_t>(width_) * static_cast<uint32_t>(height_) * 4;
  uint32_t msgHdr[2] = {htonl(static_cast<uint32_t>(WireMsgType::FRAME)),
                        htonl(payloadLen)};
  uint32_t whHdr[2] = {htonl(static_cast<uint32_t>(width_)),
                       htonl(static_cast<uint32_t>(height_))};
  std::lock_guard<std::mutex> lock(socket_write_mutex_);
  if (!writeAll(fd, msgHdr, sizeof(msgHdr)))
    return false;
  if (!writeAll(fd, whHdr, sizeof(whHdr)))
    return false;
  const size_t rowBytes = static_cast<size_t>(width_) * 4;
  for (int y = 0; y < height_; ++y) {
    const uint8_t *row = f->data[0] + static_cast<size_t>(y) * f->linesize[0];
    if (!writeAll(fd, row, rowBytes))
      return false;
  }
  return true;
}

void PeerVideoRenderer::closeConnection() {
  int fd = conn_fd_.exchange(-1);
  if (fd < 0)
    return;
  writeWireControl(fd, "CALL_END");
  ::shutdown(fd, SHUT_RDWR);
  ::close(fd);
}

// ============================================================================
// RendererLauncher
// ============================================================================
void RendererLauncher::setDebugCallback(std::function<void(std::string)> cb) {
  debug_cb_ = std::move(cb);
}

void RendererLauncher::setRenderer(std::string mode) {
  renderer_ = std::move(mode);
}
const std::string &RendererLauncher::renderer() const { return renderer_; }

bool RendererLauncher::active() const { return active_; }
int RendererLauncher::remoteListenFd() const { return remote_listen_fd_; }
int RendererLauncher::localListenFd() const { return local_listen_fd_; }
const std::string &RendererLauncher::remoteSocketPath() const {
  return remote_socket_path_;
}
const std::string &RendererLauncher::localSocketPath() const {
  return local_socket_path_;
}

void RendererLauncher::start() {
  if (active_)
    return;

  if (renderer_ == "raw") {
    std::string pidTag = std::to_string(::getpid());
    remote_socket_path_ = "/tmp/jamcli-" + pidTag + "-remote.sock";
    local_socket_path_ = "/tmp/jamcli-" + pidTag + "-local.sock";
    remote_listen_fd_ = bindUnixSocket(remote_socket_path_);
    local_listen_fd_ = bindUnixSocket(local_socket_path_);
    if (remote_listen_fd_ < 0) {
      report("Could not create a renderer socket - video view unavailable.");
      cleanupSockets();
      return;
    }
    report("renderer=raw: run --raw-renderer manually to view a stream.");
  }

  active_ = true;
  report("Video streaming started (renderer=" + renderer_ + ").");
}

void RendererLauncher::stop() {
  if (!active_)
    return;
  active_ = false;
  cleanupSockets();
  report("Video streaming ended.");
}
void RendererLauncher::requestStop() { stop(); }

RendererLauncher::~RendererLauncher() { stop(); }

void RendererLauncher::report(const std::string &msg) {
  if (debug_cb_)
    debug_cb_(msg);
}

void RendererLauncher::cleanupSockets() {
  for (int *fd : {&remote_listen_fd_, &local_listen_fd_}) {
    if (*fd >= 0) {
      ::close(*fd);
      *fd = -1;
    }
  }
  for (const auto &p : {remote_socket_path_, local_socket_path_}) {
    if (!p.empty())
      ::unlink(p.c_str());
  }
  remote_socket_path_.clear();
  local_socket_path_.clear();
}
