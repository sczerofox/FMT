package org.lazy.fmt.demo;

import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtException;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FileQuery;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtPage;
import org.lazy.fmt.client.model.FmtServerStatus;

import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.stereotype.Component;

/**
 * 启动自检：{@code --demo.smoke=true} 时打一遍「通道 → ping → status → 列文件」，
 * 用来看清「到底是服务没开、还是 token 不对」。
 *
 * <p>两条纪律：
 * <ol>
 *   <li>**绝不因为诊断失败就把应用拽崩**——连不上 FMT 只是"现在用不了 FMT"，
 *       不是"这个服务起不来"（连接失败、超时、配置缺失都在这里被兜住并打印）；</li>
 *   <li>输出里**不出现 token 明文**（通道的 {@code describe()} 已经是掩码）。</li>
 * </ol>
 */
@Component
@ConditionalOnProperty(prefix = "demo", name = "smoke", havingValue = "true")
public class FmtSmokeRunner implements ApplicationRunner {

    private final FmtChannel fmt;

    public FmtSmokeRunner(FmtChannel fmt) {
        this.fmt = fmt;
    }

    @Override
    public void run(ApplicationArguments args) {
        line("通道", fmt.describe());
        try {
            FmtResult<java.util.Map<String, Object>> ping = fmt.ping();
            line("GET /api/ping", ping.isOk() ? "ok（公开接口，不需要 token）" : ping.error().toString());

            FmtResult<FmtServerStatus> status = fmt.status();
            if (status.isFailure()) {
                line("GET /api/status", status.error().toString());
                line("提示", "服务活着但凭证不对时就是这里失败；在数据根下跑 `fmt.exe config list` "
                        + "取访问 token，或用 --fmt.data-root=<数据根> 让通道自己读 data/user.json");
                return;
            }
            line("GET /api/status", "数据根=" + status.data().root()
                    + "，版本=" + status.data().version()
                    + "，pid=" + status.data().pid());

            FmtResult<FmtPage> page = fmt.listFiles(FileQuery.all());
            if (page.isFailure()) {
                line("GET /api/file", page.error().toString());
                return;
            }
            line("GET /api/file", page.data().count() + " 个文件（共 " + page.data().total() + "）");
            for (FmtFile file : page.data().files()) {
                line("  ·", file.fileId() + "  " + file.fileName() + "  " + file.size() + " B");
            }
        } catch (FmtException failure) {
            // 连不上 / 超时 / 凭证没配：只报告，不影响应用启动
            line("自检未通过", failure.error() + "（应用照常启动，FMT 可用后再调 /demo/* 即可）");
        }
    }

    private static void line(String label, String value) {
        System.out.println("[fmt-smoke] " + label + "：" + value);
    }
}
