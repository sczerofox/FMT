package org.lazy.fmt.spring;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.lazy.fmt.client.FmtErrorCodes;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FmtConfig;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtServerStatus;
import org.lazy.fmt.client.model.FmtTrashEntry;

import org.junit.jupiter.api.Test;

import static org.assertj.core.api.Assertions.assertThat;

/**
 * SDK 的模型必须能被**宿主的** Jackson 干净地序列化——因为最常见的用法就是
 * `@GetMapping` 直接把 {@code FmtFile} / {@code FmtTrashEntry} 返回给前端。
 *
 * <p>真机教训：在 record 上加 {@code isXxx()} 助手会被 Jackson 当成 getter，
 * 与同名的 record 组件抢同一个属性名并**覆盖**真实值——`FmtTrashEntry.bucket`
 * 在真机输出里变成了 {@code false}（`isBucket()` 赢了 `bucket()`）。
 * 所以助手一律避开 getter 命名（{@code inTrash()} / {@code fileEntry()}）。
 */
class SdkModelJacksonTest {

    private final ObjectMapper mapper = new ObjectMapper();

    @Test
    void trashEntryKeepsItsBucketName() throws Exception {
        FmtTrashEntry entry = new FmtTrashEntry("file", "fmt-20261008-1", "a7.jpg", "lazy",
                "2026-10-08T19:48:30", 114724L, 1, true, true,
                "trash/user/.files/lazy/2026/10/08/a7.jpg", "");

        String json = mapper.writeValueAsString(entry);

        assertThat(json).contains("\"bucket\":\"lazy\"");
        assertThat(json).contains("\"type\":\"file\"");
        assertThat(json).doesNotContain("\"file\":");
        assertThat(json).doesNotContain("fileEntry");
        assertThat(json).doesNotContain("bucketEntry");
    }

    @Test
    void configNeverCarriesAToken() throws Exception {
        FmtConfig config = new FmtConfig("user", "lazy", 52428800L, "MB", "zh-CN",
                "config/config.json", "u-1", "2026-10-09 21:19:39", "");

        String json = mapper.writeValueAsString(config);

        assertThat(json).contains("\"currentBucket\":\"lazy\"");
        assertThat(json).doesNotContain("token");
    }

    @Test
    void fileKeepsItsTrashedState() throws Exception {
        FmtFile untrashed = new FmtFile("fmt-1", "a.jpg", "work", ".jpg", "image", 12L, "md5",
                null, "p", "", "");
        FmtFile trashed = new FmtFile("fmt-2", "b.jpg", "work", ".jpg", "image", 12L, "md5",
                Boolean.TRUE, "p", "tp", "reason");

        assertThat(mapper.writeValueAsString(untrashed)).contains("\"trashed\":null");
        assertThat(mapper.writeValueAsString(trashed)).contains("\"trashed\":true");
        assertThat(mapper.writeValueAsString(trashed)).doesNotContain("inTrash");
    }

    /**
     * {@code FmtResult} 直接返回给前端时，JSON 必须还是 FMT 的信封，而且**成功不能丢 data**。
     *
     * <p>真机踩过：Spring MVC 里 {@code return fmtService.status();}（返回
     * {@code FmtResult<FmtServerStatus>}）在 Boot 3.4.5 + Jackson 2.18 下只输出
     * {@code {"ok":true,"failure":false}} —— Jackson 按 bean 处理本类，{@code isFailure()}
     * 抛异常被吞、{@code data()} 在失败分支也抛异常，于是 data 整个消失。
     * 修法是给 {@code FmtResult} 加 {@code @JsonProperty}/{@code @JsonIgnore}/{@code @JsonInclude}。
     */
    @Test
    void resultKeepsTheEnvelopeAndNeverLosesData() throws Exception {
        FmtServerStatus status = new FmtServerStatus("D:/Tools/fmt", 4321L, "1.0.0", "http");

        String success = mapper.writeValueAsString(FmtResult.ok(status));
        String failure = mapper.writeValueAsString(
                FmtResult.failure(FmtErrorCodes.FILE_NOT_FOUND, "文件不存在：a.txt"));

        // 字段顺序按字母序（data 在 ok 前），所以断言用 contains 而不是整串相等
        assertThat(success).contains("\"ok\":true");
        assertThat(success).contains("\"data\":{");
        assertThat(success).contains("\"root\":\"D:/Tools/fmt\"");
        assertThat(success).contains("\"pid\":4321");
        assertThat(success).doesNotContain("\"error\"");
        // isFailure() 是内部助手，不能变成 JSON 字段
        assertThat(success).doesNotContain("failure");

        assertThat(failure).contains("\"ok\":false");
        assertThat(failure).contains("\"code\":\"FMT-002\"");
        assertThat(failure).contains("\"message\":");
        assertThat(failure).doesNotContain("\"data\"");
        assertThat(failure).doesNotContain("failure");
    }

    @Test
    void emptyPingResultStillSerializesAsAnEnvelope() throws Exception {
        // ping 的 data 是空 Map：不能因为 NON_NULL 就把整个 data 抹掉
        assertThat(mapper.writeValueAsString(FmtResult.ok(java.util.Map.of())))
                .contains("\"ok\":true")
                .contains("\"data\":{}");
    }
}
