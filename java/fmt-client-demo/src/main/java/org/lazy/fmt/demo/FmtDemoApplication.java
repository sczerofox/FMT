package org.lazy.fmt.demo;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.scheduling.annotation.EnableScheduling;

/**
 * 示例应用：一个既能被别的系统调用、又会主动连 FMT 的「桥接 jar」。
 *
 * <pre>{@code
 * mvn -s tools/maven-settings.xml -pl fmt-client-demo -am package
 * java -jar fmt-client-demo/target/fmt-bridge.jar \
 *      --fmt.data-root=D:/Tools/fmt --demo.smoke=true
 * }</pre>
 *
 * <p>它演示三种数据流（都建立在同一个通道上）：
 * <ol>
 *   <li><b>Java → FMT</b>：{@code POST /demo/upload} 把文件推进 FMT；</li>
 *   <li><b>FMT → Java</b>：{@code FmtInboxPoller} 定时列文件、把新文件拉回本地 inbox
 *       （FMT 没有回调/webhook，所以「FMT 推给你」只能做成「你去拉」）；</li>
 *   <li><b>Java → 外部系统</b>：{@code GET /demo/files} 这类 REST 接口，
 *       别的系统调它，它在内部用同一个通道调 FMT。</li>
 * </ol>
 */
@SpringBootApplication
@EnableScheduling
public class FmtDemoApplication {

    public static void main(String[] args) {
        SpringApplication.run(FmtDemoApplication.class, args);
    }
}
