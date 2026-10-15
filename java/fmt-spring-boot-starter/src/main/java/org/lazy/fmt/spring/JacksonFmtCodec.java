package org.lazy.fmt.spring;

import org.lazy.fmt.client.FmtCodec;
import org.lazy.fmt.client.FmtJsonException;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;

/**
 * 用宿主的 {@link ObjectMapper} 做编解码：业务代码里的 Jackson 配置（命名策略、时间格式、
 * 模块注册）能直接生效，SDK 不另起一套。
 */
public class JacksonFmtCodec implements FmtCodec {

    private final ObjectMapper mapper;

    public JacksonFmtCodec(ObjectMapper mapper) {
        this.mapper = mapper;
    }

    @Override
    public Object read(String json) {
        try {
            return mapper.readValue(json, Object.class);
        } catch (JsonProcessingException broken) {
            throw new FmtJsonException("Jackson 解析失败：" + broken.getOriginalMessage(), broken);
        }
    }

    @Override
    public String write(Object value) {
        try {
            return mapper.writeValueAsString(value);
        } catch (JsonProcessingException broken) {
            throw new FmtJsonException("Jackson 序列化失败：" + broken.getOriginalMessage(), broken);
        }
    }
}
