package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtChannel;
import org.lazy.fmt.client.FmtResult;
import org.lazy.fmt.client.model.FmtFile;
import org.lazy.fmt.client.model.FmtFilePackage;
import org.lazy.fmt.client.model.FmtMedia;
import org.lazy.fmt.client.model.FmtPayload;

import org.junit.jupiter.api.Test;
import org.springframework.core.io.Resource;
import org.springframework.http.ResponseEntity;

import java.io.IOException;
import java.io.InputStream;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicReference;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

/** FmtFileResponse：文件信息 + 预览/下载 ResponseEntity，临时文件随流关闭删除。 */
class FmtFileResponseTest {

    private static final byte[] DATA = new byte[]{1, 2, 3};

    @Test
    void previewAndDownloadCarryFileInfoAndHeaders() throws Exception {
        AtomicReference<Path> lastTarget = new AtomicReference<>();
        FmtChannel channel = fakeChannel(lastTarget);
        FmtFile file = new FmtFile("fmt-1", "a.png", "work", ".png", "image",
                DATA.length, "md5", null, "", "", "");
        FmtFilePackage pkg = new FmtFilePackage(channel, file, "a.png");
        FmtFileResponse response = FmtFileResponse.of(pkg);

        assertEquals("fmt-1", response.fileId());
        assertEquals("a.png", response.fileName());

        ResponseEntity<Resource> preview = response.preview();
        assertEquals(200, preview.getStatusCode().value());
        assertEquals("image/png", preview.getHeaders().getContentType().toString());
        assertTrue(preview.getHeaders().getFirst("Content-Disposition").startsWith("inline"));
        assertArrayEquals(DATA, readAndClose(preview.getBody()));
        assertFalse(Files.exists(lastTarget.get()), "关闭预览流后临时文件应删除");

        ResponseEntity<Resource> download = response.download();
        assertEquals(200, download.getStatusCode().value());
        assertTrue(download.getHeaders().getFirst("Content-Disposition").startsWith("attachment"));
        assertArrayEquals(DATA, readAndClose(download.getBody()));
        assertFalse(Files.exists(lastTarget.get()), "关闭下载流后临时文件应删除");
    }

    private static byte[] readAndClose(Resource resource) throws IOException {
        try (InputStream in = resource.getInputStream()) {
            return in.readAllBytes();
        }
    }

    private static FmtChannel fakeChannel(AtomicReference<Path> targetRef) {
        return (FmtChannel) Proxy.newProxyInstance(
                FmtFileResponseTest.class.getClassLoader(),
                new Class<?>[]{FmtChannel.class},
                (proxy, method, args) -> {
                    if ("preview".equals(method.getName()) || "download".equals(method.getName())) {
                        Path target = (Path) args[1];
                        Files.write(target, DATA);
                        targetRef.set(target);
                        boolean inline = "preview".equals(method.getName());
                        return FmtResult.ok(new FmtPayload(
                                target, DATA.length, "a.png",
                                inline ? "image/png" : "application/octet-stream", inline));
                    }
                    throw new UnsupportedOperationException(method.getName());
                });
    }
}
