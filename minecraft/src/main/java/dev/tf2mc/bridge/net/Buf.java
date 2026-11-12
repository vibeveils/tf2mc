package dev.tf2mc.bridge.net;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/**
 * Little-endian growable buffer used for both writing frames and reading payloads.
 * Frame layout: u32 length (type + payload), u8 type, payload. Mirrors core/mc_protocol.h.
 */
public final class Buf {
    private ByteBuffer bb;

    private Buf(ByteBuffer bb) {
        this.bb = bb.order(ByteOrder.LITTLE_ENDIAN);
    }

    /** Starts a new outgoing frame of the given packet type. */
    public static Buf frame(int type) {
        Buf b = new Buf(ByteBuffer.allocate(64));
        b.bb.putInt(0);
        b.bb.put((byte) type);
        return b;
    }

    /** Wraps a received payload (without length prefix and type byte). */
    public static Buf wrap(byte[] payload) {
        return new Buf(ByteBuffer.wrap(payload));
    }

    private void ensure(int n) {
        if (bb.remaining() >= n) return;
        int cap = Math.max(bb.capacity() * 2, bb.position() + n);
        ByteBuffer nb = ByteBuffer.allocate(cap).order(ByteOrder.LITTLE_ENDIAN);
        bb.flip();
        nb.put(bb);
        bb = nb;
    }

    public Buf u8(int v) { ensure(1); bb.put((byte) v); return this; }
    public Buf u16(int v) { ensure(2); bb.putShort((short) v); return this; }
    public Buf i32(int v) { ensure(4); bb.putInt(v); return this; }
    public Buf i64(long v) { ensure(8); bb.putLong(v); return this; }
    public Buf f32(float v) { ensure(4); bb.putFloat(v); return this; }
    public Buf f64(double v) { ensure(8); bb.putDouble(v); return this; }
    public Buf bytes(byte[] b, int off, int len) { ensure(len); bb.put(b, off, len); return this; }

    public Buf str(String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        int n = Math.min(b.length, 65535);
        u16(n);
        return bytes(b, 0, n);
    }

    /** Finalises the length prefix and returns the full frame. */
    public byte[] finish() {
        int len = bb.position() - 4;
        bb.putInt(0, len);
        byte[] out = new byte[bb.position()];
        System.arraycopy(bb.array(), 0, out, 0, out.length);
        return out;
    }

    // ---- reading ----
    public int remaining() { return bb.remaining(); }
    public int ru8() { return bb.get() & 0xFF; }
    public int rs8() { return bb.get(); }
    public int ru16() { return bb.getShort() & 0xFFFF; }
    public int ri32() { return bb.getInt(); }
    public long ri64() { return bb.getLong(); }
    public float rf32() { return bb.getFloat(); }
    public double rf64() { return bb.getDouble(); }

    public String rstr() {
        int n = ru16();
        byte[] b = new byte[n];
        bb.get(b);
        return new String(b, StandardCharsets.UTF_8);
    }
}
