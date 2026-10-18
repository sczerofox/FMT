package org.lazy.fmt.client;

import java.util.Map;

/**
 * FMT 错误码常量，与 {@code include/fmt/common/error.hpp} 的 {@code ErrorCode} 一一对应。
 *
 * <p>编号一旦发布就不复用、不改语义，因此这里是**照抄**，不做任何重排。
 * 新增错误码只能由 FMT 侧追加新编号，本类同步追加即可。
 */
public final class FmtErrorCodes {

    /** FMT-000：成功（错误码层面）。 */
    public static final String OK = "FMT-000";

    // ---- FMT-0xx 通用、JSON、路径 ----
    public static final String INVALID_ARGUMENT = "FMT-001";
    public static final String FILE_NOT_FOUND = "FMT-002";
    public static final String FILE_ALREADY_EXISTS = "FMT-003";
    public static final String PERMISSION_DENIED = "FMT-004";
    public static final String IO_ERROR = "FMT-005";
    public static final String JSON_PARSE_ERROR = "FMT-006";
    public static final String JSON_WRITE_ERROR = "FMT-007";
    public static final String CONFIG_ERROR = "FMT-008";
    public static final String STORAGE_ERROR = "FMT-009";
    public static final String MD5_ERROR = "FMT-010";
    public static final String JSON_UNSUPPORTED_VERSION = "FMT-011";
    public static final String PATH_TOO_LONG = "FMT-012";
    public static final String DIRECTORY_CREATE_FAILED = "FMT-013";
    public static final String PATH_ESCAPE = "FMT-014";
    public static final String CONSISTENCY_ERROR = "FMT-015";
    /** 需要显式确认（删除/清空类操作没带 force）。 */
    public static final String CONFIRM_REQUIRED = "FMT-016";
    /** 没有这个接口（HTTP 404）。 */
    public static final String ROUTE_NOT_FOUND = "FMT-017";
    /** 缺少或无效的访问 token（HTTP 401）——通道自愈就靠这个码。 */
    public static final String UNAUTHORIZED = "FMT-018";
    /** 访问 token 已过期（HTTP 401）；登录模式下通道会用账号密码自动重登。 */
    public static final String TOKEN_EXPIRED = "FMT-019";
    /** 账号/密码/库名登录失败（HTTP 401）。 */
    public static final String LOGIN_FAILED = "FMT-020";

    // ---- FMT-1xx 文件名校验 ----
    public static final String FILE_NAME_EMPTY = "FMT-100";
    public static final String FILE_NAME_INVALID_CHAR = "FMT-101";
    public static final String FILE_NAME_SEPARATOR = "FMT-102";
    public static final String FILE_NAME_RESERVED = "FMT-103";
    public static final String FILE_NAME_TOO_LONG = "FMT-104";
    public static final String FILE_NAME_CONFLICT = "FMT-105";
    public static final String FILE_NAME_LIKE_FILE_ID = "FMT-106";

    // ---- FMT-2xx Bucket（HTTP 侧不暴露，仅本地/管道） ----
    public static final String BUCKET_NOT_FOUND = "FMT-200";
    public static final String BUCKET_ALREADY_EXISTS = "FMT-201";
    public static final String BUCKET_NAME_INVALID = "FMT-202";
    public static final String BUCKET_IN_USE = "FMT-203";

    // ---- FMT-3xx 上传与下载 ----
    public static final String URL_INVALID = "FMT-300";
    public static final String DOWNLOAD_FAILED = "FMT-301";
    public static final String DOWNLOAD_TIMEOUT = "FMT-302";
    /** 超过 max_upload_size：服务端**边收边判**，超了立刻中止。 */
    public static final String SIZE_LIMIT_EXCEEDED = "FMT-303";
    /**
     * 同一个 Bucket 内已有**相同内容**的正常文件（「文件已存在」）：名字相同或不同都报它，
     * 而且**不要求改文件名**——换个名字也还是同一份内容。
     * 内容不同但文件名相同报 {@link #FILE_NAME_CONFLICT}（那才要改名字）。
     */
    public static final String MD5_DUPLICATE = "FMT-304";
    public static final String NO_CURRENT_BUCKET = "FMT-305";

    // ---- FMT-4xx Trash ----
    public static final String TRASH_ENTRY_NOT_FOUND = "FMT-400";
    public static final String RESTORE_CONFLICT = "FMT-401";
    public static final String RESTORE_BUCKET_MISSING = "FMT-402";

    // ---- FMT-5xx Share ----
    public static final String SHARE_NOT_FOUND = "FMT-500";
    public static final String SHARE_EXPIRED = "FMT-501";
    public static final String SHARE_DOWNLOAD_LIMIT_REACHED = "FMT-502";
    public static final String SHARE_FILE_UNAVAILABLE = "FMT-503";

    // ---- FMT-6xx Service ----
    public static final String SERVICE_ALREADY_INSTALLED = "FMT-600";
    public static final String SERVICE_NOT_INSTALLED = "FMT-601";
    /** 服务操作失败；HTTP 侧「已登记但未实现」的接口也用它（状态码 501）。 */
    public static final String SERVICE_OPERATION_FAILED = "FMT-602";
    public static final String ADMIN_REQUIRED = "FMT-603";
    public static final String NO_CURRENT_USER = "FMT-604";

    // ---- FMT-7xx HTTP ----
    public static final String HTTP_REQUEST_INVALID = "FMT-700";
    /** 该文件类型不支持预览（下载不受影响）。 */
    public static final String PREVIEW_UNSUPPORTED = "FMT-701";

    private static final Map<Integer, String> NAMES = Map.ofEntries(
            Map.entry(0, "Ok"),
            Map.entry(1, "InvalidArgument"), Map.entry(2, "FileNotFound"),
            Map.entry(3, "FileAlreadyExists"), Map.entry(4, "PermissionDenied"),
            Map.entry(5, "IoError"), Map.entry(6, "JsonParseError"),
            Map.entry(7, "JsonWriteError"), Map.entry(8, "ConfigError"),
            Map.entry(9, "StorageError"), Map.entry(10, "Md5Error"),
            Map.entry(11, "JsonUnsupportedVersion"), Map.entry(12, "PathTooLong"),
            Map.entry(13, "DirectoryCreateFailed"), Map.entry(14, "PathEscape"),
            Map.entry(15, "ConsistencyError"), Map.entry(16, "ConfirmRequired"),
            Map.entry(17, "RouteNotFound"), Map.entry(18, "Unauthorized"),
            Map.entry(19, "TokenExpired"), Map.entry(20, "LoginFailed"),
            Map.entry(100, "FileNameEmpty"), Map.entry(101, "FileNameInvalidChar"),
            Map.entry(102, "FileNameSeparator"), Map.entry(103, "FileNameReserved"),
            Map.entry(104, "FileNameTooLong"), Map.entry(105, "FileNameConflict"),
            Map.entry(106, "FileNameLikeFileId"),
            Map.entry(200, "BucketNotFound"), Map.entry(201, "BucketAlreadyExists"),
            Map.entry(202, "BucketNameInvalid"), Map.entry(203, "BucketInUse"),
            Map.entry(300, "UrlInvalid"), Map.entry(301, "DownloadFailed"),
            Map.entry(302, "DownloadTimeout"), Map.entry(303, "SizeLimitExceeded"),
            Map.entry(304, "Md5Duplicate"), Map.entry(305, "NoCurrentBucket"),
            Map.entry(400, "TrashEntryNotFound"), Map.entry(401, "RestoreConflict"),
            Map.entry(402, "RestoreBucketMissing"),
            Map.entry(500, "ShareNotFound"), Map.entry(501, "ShareExpired"),
            Map.entry(502, "ShareDownloadLimitReached"), Map.entry(503, "ShareFileUnavailable"),
            Map.entry(600, "ServiceAlreadyInstalled"), Map.entry(601, "ServiceNotInstalled"),
            Map.entry(602, "ServiceOperationFailed"), Map.entry(603, "AdminRequired"),
            Map.entry(604, "NoCurrentUser"),
            Map.entry(700, "HttpRequestInvalid"), Map.entry(701, "PreviewUnsupported"));

    /** 与 C++ 侧 {@code exit_code()} 对齐的退出码（跨语言脚本分类用）。 */
    private static final Map<Integer, Integer> EXIT_CODES = Map.ofEntries(
            Map.entry(0, 0),
            Map.entry(1, 2), Map.entry(2, 3), Map.entry(3, 4), Map.entry(4, 5),
            Map.entry(5, 1), Map.entry(6, 6), Map.entry(7, 1), Map.entry(8, 7),
            Map.entry(9, 1), Map.entry(10, 1), Map.entry(11, 6), Map.entry(12, 2),
            Map.entry(13, 1), Map.entry(14, 2), Map.entry(15, 6), Map.entry(16, 2),
            Map.entry(17, 3), Map.entry(18, 5),
            Map.entry(100, 2), Map.entry(101, 2), Map.entry(102, 2), Map.entry(103, 2),
            Map.entry(104, 2), Map.entry(105, 4), Map.entry(106, 2),
            Map.entry(200, 3), Map.entry(201, 4), Map.entry(202, 2), Map.entry(203, 4),
            Map.entry(300, 2), Map.entry(301, 1), Map.entry(302, 1), Map.entry(303, 2),
            Map.entry(304, 4), Map.entry(305, 3),
            Map.entry(400, 3), Map.entry(401, 4), Map.entry(402, 3),
            Map.entry(500, 3), Map.entry(501, 5), Map.entry(502, 5), Map.entry(503, 5),
            Map.entry(600, 8), Map.entry(601, 8), Map.entry(602, 8), Map.entry(603, 5),
            Map.entry(604, 7),
            Map.entry(700, 2), Map.entry(701, 2));

    private FmtErrorCodes() {
    }

    /** {@code 18 -> "FMT-018"}。 */
    public static String format(int number) {
        return String.format("FMT-%03d", number);
    }

    /** {@code "FMT-018" / "18" -> 18}；无法识别返回 -1。 */
    public static int number(String code) {
        if (code == null) {
            return -1;
        }
        String text = code.trim();
        if (text.regionMatches(true, 0, "FMT-", 0, 4)) {
            text = text.substring(4);
        }
        try {
            return Integer.parseInt(text);
        } catch (NumberFormatException ignored) {
            return -1;
        }
    }

    /** {@code 18 -> "Unauthorized"}；未知编号返回 {@code "Unknown"}（与 C++ 侧一致）。 */
    public static String name(int number) {
        return NAMES.getOrDefault(number, "Unknown");
    }

    /** 未知编号与 C++ 侧一样默认回落到 1（通用错误）。 */
    public static int exitCode(int number) {
        return EXIT_CODES.getOrDefault(number, 1);
    }

    public static int exitCode(String code) {
        return exitCode(number(code));
    }

    /** 是否是需要「显式确认」的错误：调用方补 force 后即可重试。 */
    public static boolean isConfirmRequired(String code) {
        return CONFIRM_REQUIRED.equals(code);
    }

    /** 是否是认证类错误：通道会尝试刷新凭证并重试一次。 */
    public static boolean isUnauthorized(String code) {
        return UNAUTHORIZED.equals(code);
    }
}
