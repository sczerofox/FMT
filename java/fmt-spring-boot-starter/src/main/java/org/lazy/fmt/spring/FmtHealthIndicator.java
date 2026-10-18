package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtException;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FmtServerStatus;

import org.springframework.boot.actuate.health.Health;
import org.springframework.boot.actuate.health.HealthIndicator;

import java.util.Map;

/**
 * Actuator 健康检查（{@code /actuator/health} 里的 {@code fmt} 项）。
 *
 * <p>两件事分开报告，避免「一个 DOWN 却不知道为什么」：
 * <ol>
 *   <li>探活走 {@code GET /api/ping}——**公开接口、不需要 token**，所以它只回答
 *       「FMT 服务活着吗、HTTP 开着吗」；</li>
 *   <li>再用 {@code GET /api/status} 验一次凭证，把 {@code authenticated} 与失败原因
 *       写进 details，这样「token 配错了」和「服务没启动」一眼分得清。</li>
 * </ol>
 */
public class FmtHealthIndicator implements HealthIndicator {

    private final FmtChannel channel;

    public FmtHealthIndicator(FmtChannel channel) {
        this.channel = channel;
    }

    @Override
    public Health health() {
        Health.Builder builder;
        FmtResult<Map<String, Object>> ping;
        try {
            ping = channel.ping();
        } catch (FmtException unreachable) {
            return Health.down()
                    .withDetail("code", unreachable.error().code())
                    .withDetail("message", unreachable.getMessage())
                    .withDetail("baseUrl", channel.baseUri().toString())
                    .withDetail("authenticated", false)
                    .build();
        }
        if (ping.isOk()) {
            builder = Health.up();
        } else {
            builder = Health.down()
                    .withDetail("code", ping.error().code())
                    .withDetail("message", ping.error().message());
        }
        builder.withDetail("baseUrl", channel.baseUri().toString());
        if (channel.bucket() != null) {
            builder.withDetail("bucket", channel.bucket());
        }

        FmtResult<FmtServerStatus> status;
        try {
            status = channel.status();
        } catch (FmtException loginFailed) {
            // 登录模式下账号/密码/Bucket 不对时，token() 会在这里抛异常；
            // 健康检查必须把它报成 DOWN，而不是把 /actuator/health 打成 500。
            return builder.withDetail("authenticated", false)
                    .withDetail("authError", loginFailed.error().code() + ": "
                            + loginFailed.getMessage())
                    .build();
        }
        if (status.isOk()) {
            builder.withDetail("authenticated", true)
                    .withDetail("root", status.data().root())
                    .withDetail("version", status.data().version())
                    .withDetail("pid", status.data().pid());
        } else {
            builder.withDetail("authenticated", false)
                    .withDetail("authError", status.error().code() + ": " + status.error().message());
        }
        return builder.build();
    }
}
