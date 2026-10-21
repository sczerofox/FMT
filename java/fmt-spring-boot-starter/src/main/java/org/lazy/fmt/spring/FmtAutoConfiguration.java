package org.lazy.fmt.spring;

import org.lazy.fmt.client.Fmt;
import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtChannelConfig;
import org.lazy.fmt.client.FmtClient;
import org.lazy.fmt.client.FmtCodec;
import org.lazy.fmt.client.FmtConfigurationException;
import org.lazy.fmt.client.FmtLogger;
import org.lazy.fmt.client.json.SimpleJson;

import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.boot.autoconfigure.AutoConfiguration;
import org.springframework.boot.autoconfigure.condition.ConditionalOnClass;
import org.springframework.boot.autoconfigure.condition.ConditionalOnMissingBean;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.boot.autoconfigure.condition.ConditionalOnWebApplication;
import org.springframework.boot.context.properties.EnableConfigurationProperties;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Configuration;

/**
 * FMT 自动配置：注入一个 {@link FmtChannel} bean，业务代码直接 {@code @Autowired} 即可。
 *
 * <p>三个刻意的设计：
 * <ol>
 *   <li><b>通道单例、由 Spring 管生命周期</b>：{@code destroyMethod = "close"}，容器关的时候
 *       线程池一起收掉；TCP 连接在整个应用生命周期里池化复用。</li>
 *   <li><b>启动不连 FMT</b>：FMT 是本地服务，可能后启动、可能没打开 HTTP。所以建 bean 时
 *       只组装配置，不发请求——连不上是运行期的事，由健康检查与日志体现。</li>
 *   <li><b>全部可覆盖</b>：自己声明 {@code FmtCodec} / {@code FmtLogger} / {@code FmtChannel}
 *       bean 就会替换默认实现（{@link ConditionalOnMissingBean}）。</li>
 * </ol>
 */
@AutoConfiguration
@ConditionalOnProperty(prefix = "fmt", name = "enabled", havingValue = "true", matchIfMissing = true)
@EnableConfigurationProperties(FmtProperties.class)
public class FmtAutoConfiguration {

    /** 把通道的日志出口接到宿主的 slf4j（logger 名固定为 {@code org.lazy.fmt.client}）。 */
    @Bean
    @ConditionalOnMissingBean
    public FmtLogger fmtLogger() {
        return new Slf4jFmtLogger();
    }

    /**
     * 有宿主的 {@code ObjectMapper} 就用它做 JSON（和业务代码共用一套配置），
     * 否则退回核心包的零依赖实现。
     */
    @Bean
    @ConditionalOnMissingBean
    @ConditionalOnClass(ObjectMapper.class)
    public FmtCodec fmtCodec(ObjectProvider<ObjectMapper> objectMappers) {
        ObjectMapper mapper = objectMappers.getIfAvailable();
        return mapper == null ? SimpleJson.INSTANCE : new JacksonFmtCodec(mapper);
    }

    /** 通道本体：地址、凭证、超时、连接数都来自 {@code fmt.*}。 */
    @Bean(destroyMethod = "close")
    @ConditionalOnMissingBean
    public FmtChannel fmtChannel(FmtProperties properties,
                                 ObjectProvider<FmtCodec> codec,
                                 ObjectProvider<FmtLogger> logger) {
        FmtChannelConfig.Builder builder = FmtChannelConfig.builder()
                .baseUrl(properties.getBaseUrl())
                .authScheme(properties.getAuthScheme())
                .connectTimeout(properties.getConnectTimeout())
                .requestTimeout(properties.getRequestTimeout())
                .streamTimeout(properties.getStreamTimeout())
                .maxConnections(properties.getMaxConnections())
                .retryOnUnauthorized(properties.isRetryOnUnauthorized())
                .idempotentRetries(properties.getIdempotentRetries())
                .headers(properties.getHeaders());
        final String username = properties.getUsername();
        final boolean hasUsername = username != null && !username.isBlank();
        final boolean hasPassword = properties.getPassword() != null
                && !properties.getPassword().isBlank();
        if (hasUsername != hasPassword) {
            throw new FmtConfigurationException(
                    "fmt.username 与 fmt.password 必须一起配置（像 MySQL 一样登录）");
        }
        if (properties.hasLogin() && properties.hasToken()) {
            throw new FmtConfigurationException(
                    "fmt.token 与 fmt.username/fmt.password 只能二选一");
        }
        if (properties.hasLogin()) {
            // 像 MySQL 一样：账号 + 密码 + 可选库名。建 bean 时不发请求，第一次业务调用才登录。
            builder.login(properties.getUsername(), properties.getPassword(), properties.getBucket());
        } else if (properties.hasToken()) {
            builder.token(properties.getToken());
            if (properties.getBucket() != null && !properties.getBucket().isBlank()) {
                builder.bucket(properties.getBucket());
            }
        } else {
            // 只支持 token 或账号密码；两者都没有就不允许“碰运气”连 fmt.exe。
            throw new FmtConfigurationException(
                    "没有配置 FMT 凭证：必须给 fmt.token，或 fmt.username + fmt.password"
                            + "（fmt.bucket 可选，不填用服务端 current_bucket）");
        }
        codec.ifAvailable(builder::codec);
        logger.ifAvailable(builder::logger);
        return Fmt.channel(builder.build());
    }

    /**
     * 业务层用的简化入口：直接注入 {@link FmtClient}，不需要先拿 {@link FmtChannel}。
     *
     * <p>它包装同一个通道（生命周期仍由 {@code fmtChannel} 管），因此不会重复建连接；
     * 新代码可以只注入 {@code FmtClient}，老代码继续注入 {@code FmtChannel}。
     */
    @Bean
    @ConditionalOnMissingBean
    public FmtClient fmtClient(FmtChannel fmtChannel) {
        return FmtClient.wrap(fmtChannel);
    }

    /**
     * Servlet Web 应用里自动注册 FMT 异常翻译：宿主不用再复制 {@code FmtExceptionAdvice}。
     *
     * <p>和健康检查一样放在嵌套配置类里、条件用字符串——宿主没引 Spring Web 时，
     * 解析外层配置的方法签名不会去链接 {@code RestControllerAdvice}，非 Web 应用照常启动。
     * 只处理 FMT 自己的异常；{@code fmt.exception-advice=false} 可关闭，声明自己的
     * {@code FmtExceptionAdvice} bean 也会覆盖默认实现。
     */
    @Configuration(proxyBeanMethods = false)
    @ConditionalOnWebApplication(type = ConditionalOnWebApplication.Type.SERVLET)
    @ConditionalOnClass(name = "org.springframework.web.bind.annotation.RestControllerAdvice")
    @ConditionalOnProperty(prefix = "fmt", name = "exception-advice", havingValue = "true", matchIfMissing = true)
    static class FmtWebConfiguration {

        @Bean
        @ConditionalOnMissingBean
        public FmtExceptionAdvice fmtExceptionAdvice() {
            return new FmtExceptionAdvice();
        }
    }

    /**
     * Actuator 在的时候顺带给一个健康检查：FMT 活着吗？凭证对不对？
     *
     * <p><b>为什么必须是嵌套类 + 字符串形式的 {@code @ConditionalOnClass}</b>：
     * actuator 是可选依赖，宿主不引它就不会有这个类。而这个 bean 方法的返回类型是
     * {@link FmtHealthIndicator}（它实现了 {@code HealthIndicator}），Spring 反射解析方法签名时
     * 会去链接 {@code HealthIndicator}，此时条件还没生效——于是宿主只要忘了引 actuator，
     * 启动就是 {@code NoClassDefFoundError: org/springframework/boot/actuate/health/HealthIndicator}。
     *
     * <p>两条保险一起上：
     * <ol>
     *   <li>这个 bean 放进只有 actuator 在时才注册的嵌套配置类——**类字面量不写注解里**，
     *       解析外层配置类的方法签名时不会碰到 actuator 的类型；</li>
     *   <li>{@code @ConditionalOnClass} 用 Class 字面量（这里安全，因为整个嵌套类都不会被加载）
     *       触发条件，字符串形式同理可用。</li>
     * </ol>
     */
    @Configuration(proxyBeanMethods = false)
    @ConditionalOnClass(name = "org.springframework.boot.actuate.health.HealthIndicator")
    static class FmtHealthConfiguration {

        @Bean
        @ConditionalOnMissingBean(name = "fmtHealthIndicator")
        public FmtHealthIndicator fmtHealthIndicator(FmtChannel fmtChannel) {
            return new FmtHealthIndicator(fmtChannel);
        }
    }
}
