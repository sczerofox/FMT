package org.lazy.fmt.client.model;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * 一次预览/下载的内容包：文件信息 + 本地临时文件 + 内容类型。
 *
 * <p>由 {@link FmtFilePackage#preview()} / {@link FmtFilePackage#download()} 生成；
 * 内容已经流式落到 {@link #path()}，Spring Boot 可以直接用
 * {@code new InputStreamResource(media.openStream())} 返回，也可以读成字节。
 *
 * <p><b>用完必须 {@link #close()}</b>（或让 {@link FmtFilePackage} 关闭），否则临时文件会留在
 * {@code java.io.tmpdir}。
 */
public final class FmtMedia implements AutoCloseable {

    private final FmtFile file;
    private final FmtPayload payload;
    private boolean closed;

    FmtMedia(FmtFile file, FmtPayload payload) {
        this.file = file;
        this.payload = payload;
    }

    /** 对应的文件记录（file_id / file_name / md5 / …）。 */
    public FmtFile file() {
        return file;
    }

    /** 本地临时文件；关闭后删除。 */
    public Path path() {
        return payload.target();
    }

    public String fileName() {
        return payload.fileName();
    }

    public String contentType() {
        return payload.contentType();
    }

    public long bytes() {
        return payload.bytes();
    }

    public boolean inline() {
        return payload.inline();
    }

    /** 打开内容流；调用方负责关闭。 */
    public InputStream openStream() throws IOException {
        ensureOpen();
        return Files.newInputStream(payload.target());
    }

    /** 小文件便捷读取；大文件请用 {@link #openStream()} 流式返回。 */
    public byte[] readBytes() throws IOException {
        ensureOpen();
        return Files.readAllBytes(payload.target());
    }

    private void ensureOpen() {
        if (closed) {
            throw new IllegalStateException("FmtMedia 已关闭");
        }
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        try {
            Files.deleteIfExists(payload.target());
        } catch (IOException ignored) {
            // 临时文件清理失败不改变业务结果；系统临时目录会自行回收。
        }
    }
}
