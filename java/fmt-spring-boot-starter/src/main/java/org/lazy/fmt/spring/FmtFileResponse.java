package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtClient;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtFilePackage;
import org.lazy.fmt.client.model.FmtMedia;
import org.springframework.core.io.InputStreamResource;
import org.springframework.core.io.Resource;
import org.springframework.http.HttpHeaders;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;

import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Spring Boot 直接可用的文件响应包：**文件基本信息 + 预览响应 + 下载响应**。
 *
 * <pre>{@code
 * try (FmtFilePackage pkg = fmt.upload(file.getInputStream(), name)) {
 *     FmtFileResponse response = FmtFileResponse.of(pkg);
 *     return response.download();       // ResponseEntity<Resource>
 * }
 * }</pre>
 *
 * <p>预览/下载响应里的临时文件会在 Spring 关闭响应流时自动删除；不要把 body 的
 * {@link Resource} 取出来长期保存。
 *
 * <p>本类在 starter 里，所以可以依赖 Spring；普通 Java 用核心包的
 * {@link FmtFilePackage} / {@link FmtMedia}，它们没有 Spring 依赖。
 */
public final class FmtFileResponse {

    private final FmtFilePackage filePackage;

    private FmtFileResponse(FmtFilePackage filePackage) {
        if (filePackage == null) {
            throw new IllegalArgumentException("FmtFilePackage 不能为空");
        }
        this.filePackage = filePackage;
    }

    /** 包装一次上传/解析返回的包。 */
    public static FmtFileResponse of(FmtFilePackage filePackage) {
        return new FmtFileResponse(filePackage);
    }

    /**
     * 给已有 file_id/文件名重新构造响应包（例如数据库里只存了头像 file_id）。
     * 会先查一次文件基本信息，预览/下载内容仍按需取。
     */
    public static FmtFileResponse of(FmtClient client, String idOrName) {
        FmtFile file = client.getFile(idOrName);
        return new FmtFileResponse(new FmtFilePackage(client.channel(), file, file.fileName()));
    }

    /** 文件基本信息（file_id / file_name / md5 / size / file_type …）。 */
    public FmtFile file() {
        return filePackage.file();
    }

    public String fileId() {
        return filePackage.fileId();
    }

    public String fileName() {
        return filePackage.fileName();
    }

    /** 预览响应：`inline`，图片/文本类由 FMT 判定；其余类型会抛 FMT-701。 */
    public ResponseEntity<Resource> preview() {
        return response(filePackage.preview(), false);
    }

    /** 下载响应：`attachment`，任何类型都可下载。 */
    public ResponseEntity<Resource> download() {
        return response(filePackage.download(), true);
    }

    private ResponseEntity<Resource> response(FmtMedia media, boolean attachment) {
        String fileName = media.fileName() == null || media.fileName().isBlank()
                ? (attachment ? "download.bin" : "preview.bin")
                : media.fileName();
        String contentType = media.contentType() == null || media.contentType().isBlank()
                ? MediaType.APPLICATION_OCTET_STREAM_VALUE
                : media.contentType();

        return ResponseEntity.ok()
                .header(HttpHeaders.CONTENT_DISPOSITION,
                        (attachment ? "attachment" : "inline")
                                + "; filename*=UTF-8''"
                                + URLEncoder.encode(fileName, StandardCharsets.UTF_8)
                                .replace("+", "%20"))
                .contentType(MediaType.parseMediaType(contentType))
                .contentLength(media.bytes())
                .body(new AutoDeleteResource(media.path()));
    }

    /**
     * 用文件路径做流式 Resource；流关闭时删除临时文件。
     * 不用 InputStreamResource 自带的流（那会在构造时打开且无法在 getInputStream 里重新打开）。
     */
    private static final class AutoDeleteResource extends InputStreamResource {

        private final Path path;

        AutoDeleteResource(Path path) {
            super(InputStream.nullInputStream());
            this.path = path;
        }

        @Override
        public String getFilename() {
            return path.getFileName().toString();
        }

        @Override
        public long contentLength() {
            try {
                return Files.size(path);
            } catch (IOException failure) {
                return -1;
            }
        }

        @Override
        public InputStream getInputStream() throws IOException {
            return new FilterInputStream(Files.newInputStream(path)) {
                @Override
                public void close() throws IOException {
                    try {
                        super.close();
                    } finally {
                        Files.deleteIfExists(path);
                    }
                }
            };
        }
    }
}
