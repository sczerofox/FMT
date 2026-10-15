package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtCodec;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.json.SimpleJson;

import org.junit.jupiter.api.Test;
import org.springframework.boot.autoconfigure.AutoConfigurations;
import org.springframework.boot.test.context.FilteredClassLoader;
import org.springframework.boot.test.context.runner.ApplicationContextRunner;

import static org.assertj.core.api.Assertions.assertThat;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class FmtAutoConfigurationTest {

    private static final String TOKEN = "1a2867378b112b8e7864218fd2fd693f";

    private final ApplicationContextRunner runner = new ApplicationContextRunner()
            .withConfiguration(AutoConfigurations.of(FmtAutoConfiguration.class));

    @Test
    void createsAChannelFromProperties() {
        runner.withPropertyValues(
                        "fmt.base-url=http://127.0.0.1:4999",
                        "fmt.token=" + TOKEN,
                        "fmt.max-connections=4")
                .run(context -> {
                    assertThat(context).hasSingleBean(FmtChannel.class);
                    FmtChannel channel = context.getBean(FmtChannel.class);
                    assertEquals("http://127.0.0.1:4999", channel.baseUri().toString());
                    assertTrue(channel.describe().contains("1a28…693f"));
                    assertFalse(channel.describe().contains(TOKEN), "描述里不能有 token 明文");
                    // 没有 ObjectMapper 时退回零依赖实现
                    assertSame(SimpleJson.INSTANCE, context.getBean(FmtCodec.class));
                });
    }

    @Test
    void fmtEnabledFalseRegistersNothing() {
        runner.withPropertyValues("fmt.enabled=false", "fmt.token=" + TOKEN)
                .run(context -> {
                    assertThat(context).doesNotHaveBean(FmtChannel.class);
                    assertThat(context).doesNotHaveBean(FmtCodec.class);
                });
    }

    @Test
    void userProvidedCodecWins() {
        FmtCodec custom = new FmtCodec() {
            @Override
            public Object read(String json) {
                return SimpleJson.INSTANCE.read(json);
            }

            @Override
            public String write(Object value) {
                return SimpleJson.INSTANCE.write(value);
            }
        };
        runner.withBean("myCodec", FmtCodec.class, () -> custom)
                .withPropertyValues("fmt.token=" + TOKEN)
                .run(context -> {
                    assertThat(context).hasSingleBean(FmtCodec.class);
                    assertSame(custom, context.getBean(FmtCodec.class));
                });
    }

    @Test
    void healthIndicatorIsRegisteredWhenActuatorIsPresent() {
        runner.withPropertyValues("fmt.token=" + TOKEN)
                .run(context -> assertThat(context).hasBean("fmtHealthIndicator"));
    }

    /**
     * 宿主没引 actuator 时必须照样起得来（只少一个健康检查）。
     *
     * <p>真机踩过：健康检查的 bean 方法返回类型是 {@link FmtHealthIndicator}（实现了
     * {@code HealthIndicator}），如果它直接写在 {@link FmtAutoConfiguration} 里，
     * Spring 反射解析方法签名时会去链接 actuator 的类型，此时 {@code @ConditionalOnClass}
     * 还没生效 → {@code NoClassDefFoundError: org/springframework/boot/actuate/health/HealthIndicator}
     * → 整个应用起不来。修法是把 bean 挪进嵌套配置类（条件用字符串），这个用例就是钉住它。
     */
    @Test
    void startsWithoutActuatorOnTheClasspath() {
        runner.withClassLoader(new FilteredClassLoader("org.springframework.boot.actuate"))
                .withPropertyValues("fmt.token=" + TOKEN)
                .run(context -> {
                    assertThat(context).hasNotFailed();
                    assertThat(context).hasSingleBean(FmtChannel.class);
                    assertThat(context).doesNotHaveBean("fmtHealthIndicator");
                });
    }

    @Test
    void missingCredentialFailsWithActionableMessageOnFirstAuthenticatedCall() {
        // 指向一个必定没人监听的端口：这样"失败原因"只可能是配置，而不是服务在不在
        runner.withPropertyValues("fmt.base-url=http://127.0.0.1:1")
                .run(context -> {
                    FmtChannel channel = context.getBean(FmtChannel.class);

                    FmtConfigurationException failure =
                            assertThrows(FmtConfigurationException.class, channel::status);
                    assertTrue(failure.getMessage().contains("fmt.token"), failure.getMessage());
                    assertTrue(failure.getMessage().contains("config list"), failure.getMessage());

                    // ping 是公开接口：不带凭证，所以它的失败原因是"连不上"而不是"没配 token"
                    Throwable pingFailure = assertThrows(RuntimeException.class, channel::ping);
                    assertFalse(pingFailure instanceof FmtConfigurationException,
                            "ping 不该因为缺凭证而失败：" + pingFailure);
                });
    }

    @Test
    void channelIsClosedWhenTheContextShutsDown() {
        runner.withPropertyValues("fmt.token=" + TOKEN, "fmt.base-url=http://127.0.0.1:1")
                .run(context -> {
                    FmtChannel channel = context.getBean(FmtChannel.class);
                    assertNotNull(channel);
                    context.close();
                    // 容器关了就 close()：再用这个通道必须立刻报错，而不是发一个半死请求
                    assertThrows(IllegalStateException.class, channel::ping);
                });
    }
}
