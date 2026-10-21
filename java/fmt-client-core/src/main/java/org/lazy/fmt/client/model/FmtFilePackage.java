package org.lazy.fmt.client.model;

import org.lazy.fmt.client.FmtApiException;
import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtErrorCodes;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * 一次「上传/解析」的统一结果包。
 *
 * <p>无论本次是：
 * <ul>
 *   <li>新上传成功；</li>
 *   <li>FMT-304（同内容已存在，复用已有记录）；</li>
 *   <li>FMT-105（同名不同内容，自动改名后重传成功）；</li>
 * </ul>
 * 都返回这个包。调用方只需要看 {@link #file()} 拿 file_id / file_name，
 * 再按需调用 {@link #preview()} / {@link #download()}。
 *
 * <p>预览和下载是**惰性**的：第一次调用才向 4122 取内容并落到临时文件；
 * 用完 {@link #close()} 清理。
 */
public final class FmtFilePackage implements AutoCloseable {

    private final FmtChannel channel;
    private final FmtFile file;
    private final String uploadedName;
    private FmtMedia preview;
    private FmtMedia download;
    private boolean closed;

    public FmtFilePackage(FmtChannel channel, FmtFile file, String uploadedName) {
        this.channel = channel;
        this.file = file;
        this.uploadedName = uploadedName;
    }

    /** 入库后的文件记录（含 file_id / file_name / md5 / size / file_type …）。 */
    public FmtFile file() {
        return file;
    }

    public String fileId() {
        return file.fileId();
    }

    public String fileName() {
        return file.fileName();
    }

    /** 本次实际上传使用的文件名；发生自动改名时，它和原始文件名不同。 */
    public String uploadedName() {
        return uploadedName;
    }

    /** 预览内容（图片/文本类）；不支持预览时抛 FMT-701。 */
    public FmtMedia preview() {
        ensureOpen();
        if (preview == null) {
            preview = fetch(true);
        }
        return preview;
    }

    /**
     * 预览内容；FMT-701（类型不支持预览）返回 {@code null}，其它错误照常抛。
     * 适合 Spring Boot 的「有预览就内联，否则走下载」。
     */
    public FmtMedia previewOrNull() {
        try {
            return preview();
        } catch (FmtApiException e) {
            if (e.error().is(FmtErrorCodes.PREVIEW_UNSUPPORTED)) {
                return null;
            }
            throw e;
        }
    }

    /** 下载内容（任何类型）。 */
    public FmtMedia download() {
        ensureOpen();
        if (download == null) {
            download = fetch(false);
        }
        return download;
    }

    private FmtMedia fetch(boolean inline) {
        Path target;
        try {
            target = Files.createTempFile("fmt-", inline ? ".preview" : ".download");
        } catch (IOException failure) {
            throw new UncheckedIOException("创建 FMT 临时文件失败", failure);
        }
        try {
            FmtPayload payload = inline
                    ? channel.preview(file.fileId(), target).orThrow()
                    : channel.download(file.fileId(), target).orThrow();
            return new FmtMedia(file, payload);
        } catch (RuntimeException failure) {
            try {
                Files.deleteIfExists(target);
            } catch (IOException ignored) {
                // 清理失败不掩盖原始错误
            }
            throw failure;
        }
    }

    private void ensureOpen() {
        if (closed) {
            throw new IllegalStateException("FmtFilePackage 已关闭");
        }
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        if (preview != null) {
            preview.close();
        }
        if (download != null) {
            download.close();
        }
    }
}
