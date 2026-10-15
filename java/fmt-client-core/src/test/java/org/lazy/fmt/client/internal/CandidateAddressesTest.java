package org.lazy.fmt.client.internal;

import org.junit.jupiter.api.Test;

import java.net.URI;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * 候选地址的计算（真机教训的固化形式）。
 *
 * <p>背景：Windows 上 {@code localhost} 同时有 {@code 127.0.0.1} 与 {@code ::1}，
 * FMT 的 {@code host: localhost} 常常只绑了 {@code ::1}，而 JDK 的 HttpClient
 * 只试第一个解析结果、不回落 —— 于是「服务活着却连接被拒」。
 * 通道据此把主机名的所有解析结果都做成候选，并在连接被拒时依次尝试。
 */
class CandidateAddressesTest {

    @Test
    void literalIpv4StaysASingleCandidate() {
        List<URI> candidates = DefaultFmtChannel.buildCandidates(URI.create("http://127.0.0.1:4122"));
        assertEquals(1, candidates.size());
        assertEquals("http://127.0.0.1:4122", candidates.get(0).toString());
    }

    @Test
    void literalIpv6StaysASingleCandidate() {
        List<URI> candidates = DefaultFmtChannel.buildCandidates(URI.create("http://[::1]:4122"));
        assertEquals(1, candidates.size(), "字面地址不需要再解析：" + candidates);
        assertEquals("http://[::1]:4122", candidates.get(0).toString());
    }

    @Test
    void hostnameKeepsTheOriginalFirstAndAddsResolvedLiterals() {
        List<URI> candidates = DefaultFmtChannel.buildCandidates(URI.create("http://localhost:4122"));
        assertTrue(candidates.size() >= 1);
        assertEquals("http://localhost:4122", candidates.get(0).toString(), "第一个永远是配置里的原样");
        for (URI candidate : candidates) {
            assertEquals(4122, candidate.getPort(), "端口必须保持一致：" + candidate);
        }
        // 本机 localhost 通常同时有 127.0.0.1 与 ::1（IPv6 关掉时只有前者，所以不断言个数）
        for (URI candidate : candidates.subList(1, candidates.size())) {
            String host = candidate.getHost();
            assertTrue(host.equals("127.0.0.1") || host.equals("[::1]") || host.contains(":"),
                    "候选应该是解析出来的字面地址：" + candidate);
        }
    }

    @Test
    void portAndPathArePreserved() {
        List<URI> candidates = DefaultFmtChannel.buildCandidates(URI.create("http://localhost:8080/base"));
        for (URI candidate : candidates) {
            assertEquals(8080, candidate.getPort());
            assertEquals("/base", candidate.getPath());
        }
    }
}
