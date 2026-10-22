package org.lazy.fmt.client;

/**
 * 大文件分片上传参数。
 *
 * <pre>{@code
 * FmtLargeUploadOptions options = FmtLargeUploadOptions.builder()
 *         .chunkSize(8L * 1024 * 1024)
 *         .progress(p -> System.out.printf("%d%%%n", p.percent()))
 *         .build();
 * }</pre>
 */
public final class FmtLargeUploadOptions {

    private final long chunkSize;
    private final String md5;
    private final FmtUploadProgressListener progress;
    private final String resumeUploadId;

    private FmtLargeUploadOptions(Builder builder) {
        this.chunkSize = builder.chunkSize;
        this.md5 = builder.md5;
        this.progress = builder.progress == null ? FmtUploadProgressListener.NONE : builder.progress;
        this.resumeUploadId = builder.resumeUploadId;
    }

    public static Builder builder() {
        return new Builder();
    }

    public static FmtLargeUploadOptions defaults() {
        return builder().build();
    }

    /** 0 表示用服务端 config.json 的 upload_chunk_size。 */
    public long chunkSize() {
        return chunkSize;
    }

    /** 整文件 MD5；null 表示客户端先算一遍（用于秒传和最终校验）。 */
    public String md5() {
        return md5;
    }

    public FmtUploadProgressListener progress() {
        return progress;
    }

    /** 非空时直接续传这个 upload_id。 */
    public String resumeUploadId() {
        return resumeUploadId;
    }

    public static final class Builder {

        private long chunkSize;
        private String md5;
        private FmtUploadProgressListener progress;
        private String resumeUploadId;

        public Builder chunkSize(long chunkSize) {
            if (chunkSize < 0) {
                throw new FmtConfigurationException("chunkSize 不能为负");
            }
            this.chunkSize = chunkSize;
            return this;
        }

        public Builder md5(String md5) {
            this.md5 = md5 == null || md5.isBlank() ? null : md5.trim();
            return this;
        }

        public Builder progress(FmtUploadProgressListener progress) {
            this.progress = progress;
            return this;
        }

        public Builder resumeUploadId(String uploadId) {
            this.resumeUploadId = uploadId == null || uploadId.isBlank() ? null : uploadId.trim();
            return this;
        }

        public FmtLargeUploadOptions build() {
            return new FmtLargeUploadOptions(this);
        }
    }
}
