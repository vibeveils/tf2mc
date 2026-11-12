package dev.tf2mc.bridge.net;

import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.List;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;

/**
 * TCP endpoint the Source game connects to. Networking runs on background threads; all
 * packets are handed to the game thread through {@link #drain}. Contains no Minecraft code
 * so it can be tested standalone (see src/test/java/.../FakeWorldServer.java).
 */
public final class BridgeServer {
    private final ServerSocket server;
    private final ConcurrentLinkedQueue<Session.Incoming> inbox = new ConcurrentLinkedQueue<>();
    private final ConcurrentLinkedQueue<Session> closed = new ConcurrentLinkedQueue<>();
    private final ConcurrentLinkedQueue<Session> opened = new ConcurrentLinkedQueue<>();
    private final List<Session> sessions = new CopyOnWriteArrayList<>();
    private final AtomicInteger ids = new AtomicInteger(1);
    private final Thread acceptor;
    private volatile boolean running = true;

    public BridgeServer(String bindHost, int port, int maxSessions) throws IOException {
        server = new ServerSocket();
        server.setReuseAddress(true);
        server.bind(new InetSocketAddress(InetAddress.getByName(bindHost), port));
        acceptor = new Thread(() -> {
            while (running) {
                try {
                    Socket s = server.accept();
                    if (sessions.size() >= maxSessions) { s.close(); continue; }
                    s.setTcpNoDelay(true);
                    s.setSendBufferSize(1 << 22);
                    Session session = new Session(ids.getAndIncrement(), s);
                    sessions.add(session);
                    opened.add(session);
                    session.start(inbox::add, sess -> { sessions.remove(sess); closed.add(sess); });
                } catch (IOException e) {
                    if (!running) break;
                }
            }
        }, "tf2bridge-accept");
        acceptor.setDaemon(true);
        acceptor.start();
    }

    public int port() { return server.getLocalPort(); }
    public List<Session> sessions() { return sessions; }

    /** Game thread: process connection events and packets. */
    public void drain(Consumer<Session> onOpen, Consumer<Session.Incoming> onPacket, Consumer<Session> onClose, int maxPackets) {
        Session s;
        while ((s = opened.poll()) != null) onOpen.accept(s);
        Session.Incoming in;
        int n = 0;
        while (n++ < maxPackets && (in = inbox.poll()) != null) onPacket.accept(in);
        while ((s = closed.poll()) != null) onClose.accept(s);
    }

    public void broadcast(byte[] frame, int role) {
        for (Session s : sessions) if (s.helloDone && (role < 0 || s.role == role)) s.send(frame);
    }

    public void close() {
        running = false;
        try { server.close(); } catch (IOException ignored) {}
        for (Session s : sessions) s.close();
    }
}
