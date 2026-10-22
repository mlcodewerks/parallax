#pragma once
#include "framebuffer_manager.hpp"
#include "rdp_tmem.hpp"
#include <memory>

class GPUTMEM {
  public:
    GPUTMEM() = default;
    ~GPUTMEM() { clear(); }
    GPUTMEM(const GPUTMEM &) = delete;
    GPUTMEM &operator=(const GPUTMEM &) = delete;
    void initialize();
    void clear();
    void release_snapshots();
    // Map entries are literals (0..255), GPU framebuffer byte offsets with
    // bit 31 set, or UINT32_MAX to retain the previous snapshot byte.
    void write(RDPState &, const std::array<std::uint32_t, 4096> &, const GLFramebuffer *);
    bool materialize(RDPState &);
    void decode(const RDPState &, unsigned tile, GLuint destination,
                const std::array<int, 4> &region);

  private:
    GLuint load_program_ = 0, decode_program_ = 0, vao_ = 0;
    GLuint map_texture_ = 0, cpu_texture_ = 0, fallback_color_ = 0, decode_fbo_ = 0;
    std::vector<std::shared_ptr<RDPTMEMBacking>> snapshots_;
    std::uint64_t serial_ = 0;
    std::array<GLint, 5> load_uniforms_{};
    std::array<GLint, 5> decode_uniforms_{};
    std::shared_ptr<RDPTMEMBacking> acquire(const RDPTMEMBacking *);
    void pipeline(GLuint);
};
