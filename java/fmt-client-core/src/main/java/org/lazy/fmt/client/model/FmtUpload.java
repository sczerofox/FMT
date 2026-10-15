package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

/**
 * 上传成功的结果。
 *
 * @param file    入库后的文件记录（含 file_id / md5 / 大小）
 * @param message 服务端给的中文提示（可读，不要用它做判断）
 */
public record FmtUpload(FmtFile file, String message) {

    public static FmtUpload from(Object dataNode) {
        return new FmtUpload(FmtFile.fromRoot(dataNode), Json.str(dataNode, "message"));
    }
}
