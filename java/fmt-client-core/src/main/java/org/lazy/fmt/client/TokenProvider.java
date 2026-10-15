package org.lazy.fmt.client;

/**
 * 访问凭证的来源。**通道在建立时拿走一个 TokenProvider，之后业务代码再也不碰 token。**
 *
 * <p>为什么是「提供者」而不是一个 String：FMT 的 token 存在数据根下的
 * {@code data/user.json} 里，服务重启、用户改密码、未来 token 轮换都会让它变化。
 * 通道只持有这个入口，需要时现取，并且能在收到 401 时让实现作废缓存重新读一次。
 *
 * <p>实现约定：
 * <ul>
 *   <li>取不到凭证时抛 {@link FmtConfigurationException}（不要把 null 交给通道）；</li>
 *   <li>{@link #invalidate()} 只清自己的缓存，不做网络调用；</li>
 *   <li>**不要**在日志里打印 token 明文，需要标识就用 {@link #maskedToken()}。</li>
 * </ul>
 */
@FunctionalInterface
public interface TokenProvider {

    /** 当前可用的 token（32 位十六进制字符串）。 */
    String token();

    /** 让缓存作废：下一次 {@link #token()} 必须重新取。通道在收到 401 后调用。 */
    default void invalidate() {
        // 默认无缓存，什么都不用做
    }

    /** 用于日志/健康检查的掩码形式，例如 {@code 1a28…693f}。 */
    default String maskedToken() {
        return TokenProviders.mask(token());
    }
}
