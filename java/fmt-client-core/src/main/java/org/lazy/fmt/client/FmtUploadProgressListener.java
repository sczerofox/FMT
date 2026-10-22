package org.lazy.fmt.client;

import org.lazy.fmt.client.model.FmtUploadProgress;

/** 大文件上传进度回调；实现必须快速返回，不要在回调里做阻塞 I/O。 */
@FunctionalInterface
public interface FmtUploadProgressListener {

    FmtUploadProgressListener NONE = progress -> {
    };

    void onProgress(FmtUploadProgress progress);
}
