package org.lazy.fmt.client.model;

import org.lazy.fmt.client.json.Json;

import java.util.Map;

/**
 * {@code GET /api/config} 的结果：服务端当前配置（**只读**）。
 *
 * <p>两个要点：
 * <ol>
 *   <li>HTTP 侧**只读**：改配置（{@code max_upload_size}）会动服务进程的全局状态，
 *       那是 CLI 的活；这里没有对应的写接口。</li>
 *   <li>**这里没有 token 字段，因为服务端根本不给**：`/api/config` 连
 *       {@code ?include_token=1} 这种开关都不提供——响应体会进日志、被代理、被贴进工单，
 *       凭证多一份副本就多一处泄漏面。要 token 请走 CLI {@code fmt.exe config list}，
 *       或直接读数据根下的 {@code data/user.json}。</li>
 * </ol>
 *
 * @param currentUser    当前用户
 * @param currentBucket  当前 Bucket（HTTP 客户端只操作它）
 * @param maxUploadSize  上传上限（字节）——上传前可以先看这个，少踩一次 {@code FMT-303}
 * @param sizeUnit       大小显示单位
 * @param language       语言
 * @param path           配置文件相对数据根的路径
 * @param userId         用户 ID
 * @param userCreatedAt  账号创建时间
 * @param lastLoginAt    最近登录时间
 */
public record FmtConfig(
        String currentUser,
        String currentBucket,
        long maxUploadSize,
        String sizeUnit,
        String language,
        String path,
        String userId,
        String userCreatedAt,
        String lastLoginAt) {

    public static FmtConfig from(Object dataNode) {
        Map<String, Object> data = Json.castMap(dataNode);
        if (data == null) {
            return new FmtConfig("", "", 0L, "", "", "", "", "", "");
        }
        return new FmtConfig(
                Json.str(data, "current_user"),
                Json.str(data, "current_bucket"),
                Json.lng(data, "max_upload_size", 0L),
                Json.str(data, "size_unit"),
                Json.str(data, "language"),
                Json.str(data, "path"),
                Json.str(data, "user_id"),
                Json.str(data, "user_created_at"),
                Json.str(data, "last_login_at"));
    }
}
