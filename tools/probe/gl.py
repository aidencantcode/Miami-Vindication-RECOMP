"""Recording fake OpenGL ES 1.1 for the headless probe.

Nothing is rendered. Functions with outputs get plausible answers modelled on the
NVIDIA Tegra 2 GLES 1.1 driver this build targeted (values are representative, not
captured from a real device); everything is counted, and texture uploads, buffer
sizes and draw calls per frame are recorded for the report.

GL arguments follow the soft-float ABI: GLfloat arrives as raw bits in core
registers / stack slots, so the generic recorder shows raw hex words.
"""
import collections
import struct

GL_VENDOR, GL_RENDERER, GL_VERSION, GL_EXTENSIONS = 0x1F00, 0x1F01, 0x1F02, 0x1F03

STRINGS = {
    GL_VENDOR: b"NVIDIA Corporation",
    GL_RENDERER: b"NVIDIA Tegra",
    GL_VERSION: b"OpenGL ES-CM 1.1",
    GL_EXTENSIONS: b" ".join([
        b"GL_ARB_texture_env_combine", b"GL_ARB_texture_env_dot3", b"GL_EXT_texture_compression_dxt1",
        b"GL_EXT_texture_compression_s3tc", b"GL_EXT_texture_format_BGRA8888", b"GL_EXT_texture_lod_bias",
        b"GL_NV_texture_compression_s3tc", b"GL_OES_blend_equation_separate", b"GL_OES_blend_func_separate",
        b"GL_OES_blend_subtract", b"GL_OES_byte_coordinates", b"GL_OES_compressed_ETC1_RGB8_texture",
        b"GL_OES_compressed_paletted_texture", b"GL_OES_depth24", b"GL_OES_draw_texture",
        b"GL_OES_EGL_image", b"GL_OES_element_index_uint", b"GL_OES_extended_matrix_palette",
        b"GL_OES_fbo_render_mipmap", b"GL_OES_fixed_point", b"GL_OES_framebuffer_object",
        b"GL_OES_mapbuffer", b"GL_OES_matrix_get", b"GL_OES_matrix_palette", b"GL_OES_packed_depth_stencil",
        b"GL_OES_point_size_array", b"GL_OES_point_sprite", b"GL_OES_query_matrix", b"GL_OES_read_format",
        b"GL_OES_rgb8_rgba8", b"GL_OES_single_precision", b"GL_OES_stencil8", b"GL_OES_texture_cube_map",
        b"GL_OES_texture_mirrored_repeat"]),
}

COMPRESSED_FORMATS = [0x83F0, 0x83F1, 0x83F2, 0x83F3, 0x8D64] + list(range(0x8B90, 0x8B9A))

INTEGERS = {
    0x0D31: [8], 0x0D32: [6], 0x0D33: [2048], 0x0D36: [16], 0x0D38: [2], 0x0D39: [2],
    0x0D3A: [2048, 2048], 0x0D50: [4], 0x0D52: [8], 0x0D53: [8], 0x0D54: [8], 0x0D55: [8],
    0x0D56: [16], 0x0D57: [8], 0x84E2: [2], 0x84E8: [2048], 0x80A8: [0], 0x80A9: [0],
    0x86A2: [len(COMPRESSED_FORMATS)], 0x86A3: COMPRESSED_FORMATS,
    0x8B9A: [0x1401], 0x8B9B: [0x1908],  # IMPLEMENTATION_COLOR_READ_TYPE/FORMAT = UNSIGNED_BYTE/RGBA
    0x0B12: [1, 64], 0x846D: [1, 64], 0x846E: [1, 8],
}

FORMAT_NAMES = {0x83F0: "DXT1_RGB", 0x83F1: "DXT1_RGBA", 0x83F2: "DXT3", 0x83F3: "DXT5", 0x8D64: "ETC1",
                0x8C00: "PVRTC_4BPP_RGB", 0x8C01: "PVRTC_2BPP_RGB", 0x8C02: "PVRTC_4BPP_RGBA",
                0x8C03: "PVRTC_2BPP_RGBA", 0x1906: "ALPHA", 0x1907: "RGB", 0x1908: "RGBA",
                0x1909: "LUMINANCE", 0x190A: "LUMINANCE_ALPHA"}
TYPE_NAMES = {0x1401: "UNSIGNED_BYTE", 0x8363: "UNSIGNED_SHORT_5_6_5", 0x8033: "UNSIGNED_SHORT_4_4_4_4",
              0x8034: "UNSIGNED_SHORT_5_5_5_1"}
IDENTITY = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0]


class FakeGL:
    def __init__(self, emu):
        self.emu = emu
        self.calls = collections.Counter()
        self.first_args = {}
        self.ids = collections.Counter()
        self.strings = {}
        self.bound = {}
        self.buffer_size = {}
        self.mapped = {}
        self.active_unit = 0
        self.bound_tex = {}
        self.textures = {}
        self.uploads = collections.Counter()
        self.compressed_bytes = 0
        self.frame = -1
        self.draws = collections.Counter()
        self.clears = collections.Counter()
        self.unknown_queries = collections.Counter()
        self.array_types = collections.Counter()   # (array, size, type) -> calls
        self.draw_kinds = collections.Counter()    # (call, mode, index type) -> calls
        for imp in emu.lib.imports:
            if imp.name.startswith("gl"):
                emu.handlers[imp.name] = getattr(self, "h_" + imp.name, self.record)

    def begin_frame(self, i):
        self.frame = i

    def record(self, emu, name, a):
        self.calls[name] += 1
        if name not in self.first_args:
            self.first_args[name] = a.raw()
        return 0

    # -- queries --------------------------------------------------------------
    def h_glGetError(self, emu, name, a):
        self.calls[name] += 1
        return 0

    def h_glGetString(self, emu, name, a):
        self.calls[name] += 1
        which = a.u32()
        if which not in STRINGS:
            self.unknown_queries[f"glGetString({which:#x})"] += 1
            return 0
        if which not in self.strings:
            p = emu.heap_alloc(len(STRINGS[which]) + 1)
            emu.write(p, STRINGS[which] + b"\0")
            self.strings[which] = p
        return self.strings[which]

    def _query(self, pname):
        if pname in INTEGERS:
            return INTEGERS[pname]
        if pname == 0x0BA2:                      # GL_VIEWPORT
            return [0, 0, self.emu.screen[0], self.emu.screen[1]]
        if pname == 0x8069:                      # GL_TEXTURE_BINDING_2D
            return [self.bound_tex.get(self.active_unit, 0)]
        if pname in (0x8894, 0x8895):            # ARRAY / ELEMENT_ARRAY_BUFFER_BINDING
            return [self.bound.get(0x8892 if pname == 0x8894 else 0x8893, 0)]
        if pname in (0x8CA6, 0x8CA7):            # FRAMEBUFFER / RENDERBUFFER_BINDING_OES
            return [self.bound.get(0x8D40 if pname == 0x8CA6 else 0x8D41, 0)]
        return None

    def h_glGetIntegerv(self, emu, name, a):
        self.calls[name] += 1
        pname, ptr = a.u32(), a.u32()
        vals = self._query(pname)
        if vals is None:
            self.unknown_queries[f"glGetIntegerv({pname:#x})"] += 1
            vals = [0]
        emu.write(ptr, struct.pack(f"<{len(vals)}i", *vals))
        return 0

    def h_glGetFloatv(self, emu, name, a):
        self.calls[name] += 1
        pname, ptr = a.u32(), a.u32()
        if pname in (0x0BA6, 0x0BA7, 0x0BA8):     # MODELVIEW / PROJECTION / TEXTURE matrix
            self.unknown_queries[f"glGetFloatv({pname:#x}) matrix (returned identity)"] += 1
            vals = IDENTITY
        else:
            iv = self._query(pname)
            if iv is None:
                self.unknown_queries[f"glGetFloatv({pname:#x})"] += 1
                iv = [0]
            vals = [float(v) for v in iv]
        emu.write(ptr, struct.pack(f"<{len(vals)}f", *vals))
        return 0

    def h_glCheckFramebufferStatusOES(self, emu, name, a):
        self.calls[name] += 1
        return 0x8CD5  # GL_FRAMEBUFFER_COMPLETE_OES

    # -- object names ---------------------------------------------------------------
    def _gen(self, emu, name, a):
        self.calls[name] += 1
        n, ptr = a.s32(), a.u32()
        kind = name[5:].replace("OES", "")
        out = []
        for _ in range(max(n, 0)):
            self.ids[kind] += 1
            out.append(self.ids[kind])
        if out:
            emu.write(ptr, struct.pack(f"<{len(out)}I", *out))
        return 0

    h_glGenTextures = h_glGenBuffers = h_glGenFramebuffersOES = h_glGenRenderbuffersOES = _gen

    def h_glActiveTexture(self, emu, name, a):
        self.calls[name] += 1
        self.active_unit = a.u32() - 0x84C0
        return 0

    def h_glBindTexture(self, emu, name, a):
        self.calls[name] += 1
        a.u32()
        self.bound_tex[self.active_unit] = a.u32()
        return 0

    def _bind(self, emu, name, a):
        self.calls[name] += 1
        target, obj = a.u32(), a.u32()
        self.bound[target] = obj
        return 0

    h_glBindBuffer = h_glBindFramebufferOES = h_glBindRenderbufferOES = _bind

    # -- uploads --------------------------------------------------------------------
    def h_glTexImage2D(self, emu, name, a):
        self.calls[name] += 1
        target, level, ifmt, w, h, border, fmt, typ, pixels = (a.u32() for _ in range(9))
        key = f"{FORMAT_NAMES.get(fmt, hex(fmt))}/{TYPE_NAMES.get(typ, hex(typ))}"
        self.uploads[key] += 1
        if level == 0:
            self.textures[self.bound_tex.get(self.active_unit, 0)] = (key, w, h)
        return 0

    def h_glCompressedTexImage2D(self, emu, name, a):
        self.calls[name] += 1
        target, level, ifmt, w, h, border, size, data = (a.u32() for _ in range(8))
        key = FORMAT_NAMES.get(ifmt, hex(ifmt))
        self.uploads["compressed " + key] += 1
        self.compressed_bytes += size
        if level == 0:
            self.textures[self.bound_tex.get(self.active_unit, 0)] = ("compressed " + key, w, h)
        return 0

    def h_glBufferData(self, emu, name, a):
        self.calls[name] += 1
        target, size = a.u32(), a.u32()
        self.buffer_size[self.bound.get(target, 0)] = size
        return 0

    def h_glMapBufferOES(self, emu, name, a):
        self.calls[name] += 1
        target = a.u32()
        buf = self.bound.get(target, 0)
        size = self.buffer_size.get(buf, 0)
        p, cap = self.mapped.get(buf, (0, 0))
        if cap < size:
            p, cap = emu.heap_alloc(size), size
            self.mapped[buf] = (p, cap)
        return p

    def h_glUnmapBufferOES(self, emu, name, a):
        self.calls[name] += 1
        return 1

    def h_glReadPixels(self, emu, name, a):
        self.calls[name] += 1
        x, y, w, h, fmt, typ, ptr = (a.u32() for _ in range(7))
        if ptr and w * h:
            emu.write(ptr, b"\0" * (w * h * 4))
        return 0

    # -- per-frame statistics ------------------------------------------------------------
    TYPES = {0x1400: "BYTE", 0x1401: "UNSIGNED_BYTE", 0x1402: "SHORT", 0x1403: "UNSIGNED_SHORT",
             0x1405: "UNSIGNED_INT", 0x1406: "FLOAT", 0x140C: "FIXED"}
    MODES = {0: "POINTS", 1: "LINES", 2: "LINE_LOOP", 3: "LINE_STRIP", 4: "TRIANGLES",
             5: "TRIANGLE_STRIP", 6: "TRIANGLE_FAN"}

    def _pointer(self, emu, name, a):
        self.calls[name] += 1
        if name == "glNormalPointer":
            typ = a.u32()
            size = 3
        else:
            size, typ = a.u32(), a.u32()
        self.array_types[(name[2:-7], size, self.TYPES.get(typ, hex(typ)))] += 1
        return 0

    h_glVertexPointer = h_glTexCoordPointer = h_glColorPointer = h_glNormalPointer = _pointer

    def h_glDrawArrays(self, emu, name, a):
        self.calls[name] += 1
        self.draws[self.frame] += 1
        self.draw_kinds[("DrawArrays", self.MODES.get(a.u32(), "?"), "-")] += 1
        return 0

    def h_glDrawElements(self, emu, name, a):
        self.calls[name] += 1
        self.draws[self.frame] += 1
        mode, count, typ = a.u32(), a.u32(), a.u32()
        self.draw_kinds[("DrawElements", self.MODES.get(mode, "?"), self.TYPES.get(typ, hex(typ)))] += 1
        return 0

    def h_glClear(self, emu, name, a):
        self.calls[name] += 1
        self.clears[self.frame] += 1
        return 0
