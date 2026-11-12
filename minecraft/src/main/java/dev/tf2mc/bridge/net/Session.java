package dev.tf2mc.bridge.net;

import java.io.BufferedInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.Socket;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;

/** One TCP connection from a Source game (server.dll = authority, client.dll = observer). */
public final class Session {
    private static final byte[] POISON = new byte[0];
    private static final int MAX_FRAME = 4 * 1024 * 1024;

    public final int id;
    private final Socket socket;
    private final LinkedBlockingQueue<byte[]> outbox = new LinkedBlockingQueue<>();
    private final AtomicLong queuedBytes = new AtomicLong();
    private volatile boolean open = true;

    // Set by HELLO (game thread only)
    public int role = -1;
    public String name = "?";
    public int subscribedSlot = -1;
    public boolean helloDone;

    /** Arbitrary per-session state owned by the game-side controller. */
    public Object attachment;

    Session(int id, Socket socket) {
        this.id = id;
        this.socket = socket;
    }

    public boolean isOpen() { return open; }
    public long queuedBytes() { return queuedBytes.get(); }
    public String remote() { return String.valueOf(socket.getRemoteSocketAddress()); }

    public void send(byte[] frame) {
        if (!open) return;
        queuedBytes.addAndGet(frame.length);
        outbox.add(frame);
    }

    public void close() {
        if (!open) return;
        open = false;
        outbox.add(POISON);
        try { socket.close(); } catch (IOException ignored) {}
    }

    void start(Consumer<Incoming> sink, Consumer<Session> onClose) {
        Thread reader = new Thread(() -> {
            try (DataInputStream in = new DataInputStream(new BufferedInputStream(socket.getInputStream(), 1 << 16))) {
                while (open) {
                    int len = Integer.reverseBytes(in.readInt());
                    if (len <= 0 || len > MAX_FRAME) throw new IOException("bad frame length " + len);
                    int type = in.readUnsignedByte();
                    byte[] payload = new byte[len - 1];
                    in.readFully(payload);
                    sink.accept(new Incoming(this, type, payload));
                }
            } catch (IOException e) {
                // disconnect
            } finally {
                close();
                onClose.accept(this);
            }
        }, "tf2bridge-read-" + id);
        reader.setDaemon(true);

        Thread writer = new Thread(() -> {
            try {
                OutputStream out = socket.getOutputStream();
                byte[] batch = new byte[1 << 16];
                while (open) {
                    byte[] f = outbox.poll(1, TimeUnit.SECONDS);
                    if (f == null) continue;
                    if (f == POISON) break;
                    // Coalesce small frames into one write
                    int used = 0;
                    do {
                        queuedBytes.addAndGet(-f.length);
                        if (f.length > batch.length - used) {
                            if (used > 0) { out.write(batch, 0, used); used = 0; }
                            if (f.length > batch.length) { out.write(f); f = outbox.poll(); continue; }
                        }
                        System.arraycopy(f, 0, batch, used, f.length);
                        used += f.length;
                        f = outbox.poll();
                    } while (f != null && f != POISON);
                    if (used > 0) out.write(batch, 0, used);
                    out.flush();
                    if (f == POISON) break;
                }
            } catch (IOException | InterruptedException e) {
                // disconnect
            } finally {
                close();
            }
        }, "tf2bridge-write-" + id);
        writer.setDaemon(true);

        reader.start();
        writer.start();
    }

    /** A received frame, queued until the game thread drains it. */
    public record Incoming(Session session, int type, byte[] payload) {
        public Buf buf() { return Buf.wrap(payload); }
    }
}
