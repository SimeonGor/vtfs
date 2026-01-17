package vtfs

import org.springframework.web.bind.annotation.*

data class ApiResp<T>(
    val code: Int,
    val message: String? = null,
    val data: T? = null
)

@RestController
@RequestMapping("/fs")
class FsController {
    private val fs = FsService(tokenValue = "TODO_TOKEN")

    private fun <T> ok(data: T) = ApiResp(code = FsService.OK, data = data)
    private fun <T> err(e: FsException) = ApiResp<T>(code = e.code, message = e.message)

    @RequestMapping("/root", method = [RequestMethod.GET, RequestMethod.POST])
    fun root(@RequestParam token: String?): ApiResp<Map<String, Any>> =
        try { ok(mapOf("root" to fs.rootIno)) } catch (e: FsException) { err(e) }

    @RequestMapping("/list", method = [RequestMethod.GET, RequestMethod.POST])
    fun list(
        @RequestParam token: String?,
        @RequestParam dir: Long
    ): ApiResp<Map<String, Long>> =
        try { ok(fs.list(token, dir)) } catch (e: FsException) { err(e) }

    @RequestMapping("/lookup", method = [RequestMethod.GET, RequestMethod.POST])
    fun lookup(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ): ApiResp<Map<String, Long>> =
        try { ok(mapOf("ino" to fs.lookup(token, parent, name))) } catch (e: FsException) { err(e) }

    @RequestMapping("/create", method = [RequestMethod.GET, RequestMethod.POST])
    fun create(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ): ApiResp<Map<String, Long>> =
        try { ok(mapOf("ino" to fs.create(token, parent, name))) } catch (e: FsException) { err(e) }

    @RequestMapping("/mkdir", method = [RequestMethod.GET, RequestMethod.POST])
    fun mkdir(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ): ApiResp<Map<String, Long>> =
        try { ok(mapOf("ino" to fs.mkdir(token, parent, name))) } catch (e: FsException) { err(e) }

    @RequestMapping("/unlink", method = [RequestMethod.GET, RequestMethod.POST])
    fun unlink(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ): ApiResp<Unit> =
        try { fs.unlink(token, parent, name); ok(Unit) } catch (e: FsException) { err(e) }

    @RequestMapping("/rmdir", method = [RequestMethod.GET, RequestMethod.POST])
    fun rmdir(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ): ApiResp<Unit> =
        try { fs.rmdir(token, parent, name); ok(Unit) } catch (e: FsException) { err(e) }

    @RequestMapping("/read", method = [RequestMethod.GET, RequestMethod.POST])
    fun read(
        @RequestParam token: String?,
        @RequestParam ino: Long,
        @RequestParam offset: Int,
        @RequestParam len: Int
    ): ApiResp<Map<String, Any>> =
        try {
            val bytes = fs.read(token, ino, offset, len)
            ok(mapOf("dataB64" to fs.b64encode(bytes), "read" to bytes.size))
        } catch (e: FsException) { err(e) }

    @RequestMapping("/write", method = [RequestMethod.GET, RequestMethod.POST])
    fun write(
        @RequestParam token: String?,
        @RequestParam ino: Long,
        @RequestParam offset: Int,
        @RequestParam dataB64: String
    ): ApiResp<Map<String, Any>> =
        try {
            val n = fs.write(token, ino, offset, fs.b64decode(dataB64))
            ok(mapOf("written" to n))
        } catch (e: FsException) { err(e) }

    @RequestMapping("/link", method = [RequestMethod.GET, RequestMethod.POST])
    fun link(
        @RequestParam token: String?,
        @RequestParam oldIno: Long,
        @RequestParam newParent: Long,
        @RequestParam newName: String
    ): ApiResp<Unit> =
        try { fs.link(token, oldIno, newParent, newName); ok(Unit) }
        catch (e: FsException) { err(e) }
}
