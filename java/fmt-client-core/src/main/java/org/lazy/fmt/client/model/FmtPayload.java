package org.lazy.fmt.client.model;

import java.nio.file.Path;

/**
 * 一次「把文件取下来」的结果。
 *
 * <p>下载与预览共用：内容都是流式落盘，区别只是服务端给的 {@code Content-Disposition}
 * 是 {@code attachment}（下载）还是 {@code inline}（预览）。预览只支持图片与文本类，
 * 其余类型服务端返回 {@code FMT-701}——**下载不受这个限制**。
 *
 * @param target      落盘后的目标文件（预览时也要求给一个目标路径）
 * @param bytes       实际写入的字节数
 * @param fileName    服务端给的文件名（从 Content-Disposition 解析，优先 filename*）
 * @param contentType 服务端给的 Content-Type
 * @param inline      是否是预览（inline）
 */
public record FmtPayload(Path target, long bytes, String fileName, String contentType, boolean inline) {
}
