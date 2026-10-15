package org.lazy.fmt.demo;

import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtPayload;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/**
 * <b>FMT → Java 的那半条链路。</b>
 *
 * <p>FMT 没有回调/webhook/长连接，所以「FMT 有新文件就通知我」在协议上不存在。
 * 唯一诚实的做法是**由 Java 侧定时拉**：
 * <pre>
 * 每 60 秒 → GET /api/file（按 file_id 排序）→ 对比本地游标 → 新文件才 GET /api/file/&lt;id&gt;/download
 * </pre>
 * 游标（已见过的 file_id）落在 inbox 下的 {@code .fmt-cursor}，所以重启不会重复拉。
 *
 * <p>想让它真推送？那需要给 fmt.exe 加出站 webhook（见设计文档「演进」一节），
 * 不要试图在客户端用轮询伪装成推送而不管去重和顺序。
 */
@Component
@ConditionalOnProperty(prefix = "demo", name = "pull-enabled", havingValue = "true")
public class FmtInboxPoller {

    private static final Logger LOG = LoggerFactory.getLogger(FmtInboxPoller.class);

    private final FmtChannel fmt;
    private final Path inbox;
    private final Path cursor;
    private final Set<String> seen = ConcurrentHashMap.newKeySet();

    public FmtInboxPoller(FmtChannel fmt, @Value("${demo.inbox-dir:./inbox}") String inboxDir)
            throws IOException {
        this.fmt = fmt;
        this.inbox = Path.of(inboxDir).toAbsolutePath().normalize();
        Files.createDirectories(inbox);
        this.cursor = inbox.resolve(".fmt-cursor");
        if (Files.isRegularFile(cursor)) {
            for (String line : Files.readAllLines(cursor, StandardCharsets.UTF_8)) {
                if (!line.isBlank()) {
                    seen.add(line.trim());
                }
            }
        }
        LOG.info("FMT 收件箱：{}（已记录 {} 个文件）", inbox, seen.size());
    }

    @Scheduled(initialDelayString = "${demo.pull-initial-delay-ms:5000}",
            fixedDelayString = "${demo.pull-interval-ms:60000}")
    public void pull() {
        FmtResult<FmtPage> listed = fmt.listFiles(FileQuery.builder().sort(FileQuery.Sort.ID).build());
        if (!listed.isOk()) {
            LOG.warn("从 FMT 列文件失败，本轮跳过：{}", listed.error());
            return;
        }
        int pulled = 0;
        for (FmtFile file : listed.data().files()) {
            if (!seen.add(file.fileId())) {
                continue;
            }
            Path target = uniqueTarget(file.fileName());
            FmtResult<FmtPayload> download = fmt.download(file.fileId(), target, false);
            if (download.isOk()) {
                pulled++;
                remember(file.fileId());
                LOG.info("从 FMT 拉回 {} -> {}（{} 字节）",
                        file.fileName(), target, download.data().bytes());
            } else {
                seen.remove(file.fileId());
                LOG.warn("拉取 {} 失败，下轮再试：{}", file.fileName(), download.error());
            }
        }
        if (pulled > 0) {
            LOG.info("本轮从 FMT 拉回 {} 个文件到 {}", pulled, inbox);
        }
    }

    private Path uniqueTarget(String fileName) {
        String safe = fileName == null || fileName.isBlank()
                ? "unnamed.bin"
                : fileName.replaceAll("[\\\\/:*?\"<>|]", "_");
        Path candidate = inbox.resolve(safe);
        int index = 1;
        while (Files.exists(candidate)) {
            candidate = inbox.resolve(index + "-" + safe);
            index++;
        }
        return candidate;
    }

    private synchronized void remember(String fileId) {
        try {
            Files.writeString(cursor, fileId + System.lineSeparator(), StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException failure) {
            // 游标写不进去只影响「下次会不会重复拉」，不该让整轮拉取失败
            LOG.warn("写游标失败（下次可能重复拉同一个文件）：{}", failure.toString());
        }
    }
}
