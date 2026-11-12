// Fidelity audit of the Vulkan NVN replayer (vkb_audit.c): every place where the replay substitutes something
// (zeros, an empty texture, a default shader variant) or skips work is counted with its reason, per frame and per
// shader. TOTK_AUDIT=<present>:<presents>[,...]: a report per window (markdown table) in the log and in
// diag/audit-<present>.md. The table decides which replay mechanisms to complete first.
#pragma once
#include <stdint.h>

enum {
    A_DRAW_NO_PROGRAM,       // draw / dispatch skipped: no program or no translation of a shader
    A_DRAW_TOPOLOGY,         // draw skipped: primitive type without a Vulkan topology (line loop, quads)
    A_DRAW_NO_TARGETS,       // draw skipped: its render targets cannot be attached
    A_DRAW_NO_INDEX,         // draw skipped: index buffer outside the imported pools
    A_SHADER_UNSUPPORTED,    // draw skipped: a shader resource outside the universal layout
    A_RT_NOT_ATTACHABLE,     // a render target left out of the pass (compressed / unsupported format)
    A_TEX_UNBOUND,           // a sampled texture slot with no texture handle: empty texture
    A_TEX_SEPARATE,          // separate texture / sampler handles (not resolved yet): empty texture
    A_TEX_NO_IMAGE,          // the handle names no image (format or target not created): empty texture
    A_TEX_NOT_READY,         // the image is not in a sampled layout (e.g. bound as a target of this pass): empty texture
    A_TEX_NOT_UPLOADED,      // the contents come from game memory but cannot be uploaded (depth / 3-component formats)
    A_TEX_VIEW_ADJUSTED,     // the shader's texture type does not fit the image (cube of < 6 layers, 3D of a 2D image)
    A_IMG_MISSING,           // a storage image slot with no usable image: empty image
    A_UBO_MISSING,           // uniform buffer outside the imported pools: zeros
    A_UBO_COPIED,            // uniform buffer misaligned for this GPU: a copy of the current contents
    A_SSBO_MISSING,          // storage buffer outside the imported pools / misaligned / size 0: empty buffer
    A_SSBO_UNKNOWN_SOURCE,   // storage buffer whose address source the translation could not tell
    A_VBUF_MISSING,          // vertex buffer outside the imported pools (virtual pools): zeros
    A_ATTR_INT_AS_FLOAT,     // integer vertex attribute given as float (no variant could be made)
    A_FS_INPUT_UNWRITTEN,    // the fragment shader reads an input the vertex shader does not write (undefined)
    A_MULTISAMPLE,           // multisampled state or target replayed single-sampled
    A_RENDER_DISABLED,       // draw recorded while rendering was disabled / conditional (replayed anyway)
    A_CLEAR_MASKED,          // clear with a channel mask (not replayed)
    A_COPY_SKIPPED,          // buffer copy outside the imported pools (skipped)
    A_UPDATE_SKIPPED,        // inline uniform update not 4-byte aligned (skipped)
    A_BORDER_APPROX,         // sampler border colour approximated by a standard one
    A_OP_NOT_REPLAYED,       // a recorded command the replay does not handle
    A_TEX_COPY_UNSUPPORTED,
    A_SSBO_HOST_WRITE,
    A_MIRROR_EVICTED,        // a mirror sent back early: an access straddled its bounds       // a written storage buffer left in host memory (no device-local mirror): slow over PCIe  // a texture copy in a form not replayed (ASTC / depth / 3-component formats, scaled depth)
    A_COUNT
};

void vkb_audit(int reason, const char *key);   // key: the shader key (or NULL)
void vkb_audit_draw_begin(void);               // a draw / dispatch starts: reasons from here count it once each
void vkb_audit_present(uint64_t present);
