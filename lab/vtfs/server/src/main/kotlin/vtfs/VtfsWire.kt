package vtfs

import org.springframework.http.HttpHeaders
import org.springframework.http.MediaType
import org.springframework.http.ResponseEntity
import java.nio.ByteBuffer
import java.nio.ByteOrder

object VtfsWire {
    fun pack(ret: Long, json: String): ResponseEntity<ByteArray> {
        val jsonBytes = json.toByteArray(Charsets.UTF_8)
        val body = ByteArray(8 + jsonBytes.size)

        ByteBuffer.wrap(body)
            .order(ByteOrder.LITTLE_ENDIAN)
            .putLong(ret)

        System.arraycopy(jsonBytes, 0, body, 8, jsonBytes.size)

        return ResponseEntity.ok()
            .contentType(MediaType.APPLICATION_OCTET_STREAM)
            .contentLength(body.size.toLong())
            .header(HttpHeaders.CONNECTION, "close")
            .body(body)
    }

    fun ok(json: String): ResponseEntity<ByteArray> = pack(0L, json)
}
