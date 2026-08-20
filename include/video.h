#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <jami/videomanager_interface.h>   // libjami::SinkTarget, FrameBuffer

extern "C" {
#include <libavutil/frame.h>
}

// ============================================================================
// PeerVideoRenderer
// ============================================================================
class PeerVideoRenderer {
public:
    ~PeerVideoRenderer();

    void setDebugCallback(std::function<void(std::string)> cb);

    bool start(const std::string& sinkId, int width, int height, const std::string& peerLabel,
               const std::string& role, const std::string& rendererMode, int listenFd);

    const std::string& lastError() const;

    void pause();
    void resume();
    bool isPaused() const;

    void requestStop();
    void stopBlocking();

private:
    bool attachSinkTarget();
    libjami::FrameBuffer allocFrame();
    void enqueueFrame(libjami::FrameBuffer buf);
    void writerLoop();
    void writerLoopRawSocket();
    void writerLoopPlayerPipe();
    bool writePixelsToPipe(int fd, AVFrame* f);
    bool writeFrameToSocket(AVFrame* f);
    void closeConnection();

    int width_ {0};
    int height_ {0};
    std::string sink_id_;
    std::string role_;
    std::string renderer_mode_ {"ffplay"};
    std::string peer_label_;
    std::atomic<bool> registered_ {false};
    std::atomic<bool> paused_ {false};

    int listen_fd_ {-1};
    std::atomic<int> conn_fd_ {-1};
    std::atomic<pid_t> player_pid_ {-1};
    std::mutex socket_write_mutex_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    libjami::FrameBuffer pending_frame_;
    std::atomic<bool> writer_running_ {false};
    std::thread writer_thread_;

    std::atomic<int> frames_received_ {0};
    std::atomic<bool> watchdog_running_ {false};
    std::thread watchdog_thread_;
    std::function<void(std::string)> debug_cb_;

    std::string last_error_;
};

// ============================================================================
// RendererLauncher
// ============================================================================
class RendererLauncher {
public:
    void setDebugCallback(std::function<void(std::string)> cb);

    void setRenderer(std::string mode);
    const std::string& renderer() const;

    bool active() const;
    int remoteListenFd() const;
    int localListenFd() const;
    const std::string& remoteSocketPath() const;
    const std::string& localSocketPath() const;

    void start();
    void stop();
    void requestStop();

    ~RendererLauncher();

private:
    void report(const std::string& msg);
    void cleanupSockets();

    bool active_ {false};
    std::string remote_socket_path_, local_socket_path_;
    int remote_listen_fd_ {-1}, local_listen_fd_ {-1};
    std::string renderer_ {"ffplay"};

    std::function<void(std::string)> debug_cb_;
};
