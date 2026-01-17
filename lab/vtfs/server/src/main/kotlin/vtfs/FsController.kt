package vtfs

import org.springframework.web.bind.annotation.GetMapping
import org.springframework.web.bind.annotation.RequestMapping
import org.springframework.web.bind.annotation.RequestParam
import org.springframework.web.bind.annotation.RestController

@RestController
@RequestMapping("/fs")
class FsController {
    private val fs = FsService(tokenValue = "TODO_TOKEN")

    // минимальное JSON-экранирование строк (для message и имён)
    private fun jesc(s: String): String =
        buildString(s.length + 8) {
            for (ch in s) {
                when (ch) {
                    '\\' -> append("\\\\")
                    '"'  -> append("\\\"")
                    '\n' -> append("\\n")
                    '\r' -> append("\\r")
                    '\t' -> append("\\t")
                    else -> append(ch)
                }
            }
        }

    private fun ok(dataJson: String): Any =
        VtfsWire.ok("""{"code":0,"message":null,"data":$dataJson}""")

    private fun err(e: FsException): Any {
        val msg = jesc(e.message ?: "error")
        return VtfsWire.ok("""{"code":${e.code},"message":"$msg","data":null}""")
    }

    @GetMapping("/root")
    fun root(@RequestParam token: String?) =
        try {
            ok("""{"ino":${fs.rootIno}}""")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/list")
    fun list(
        @RequestParam token: String?,
        @RequestParam dir: Long
    ) =
        try {
            val m = fs.list(token, dir) // name -> ino
            val pairs = m.entries.joinToString(",") { (name, ino) ->
                "\"${jesc(name)}\":$ino"
            }
            ok("{${pairs}}") // важно: data должен быть объектом
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/lookup")
    fun lookup(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ) =
        try {
            val ino = fs.lookup(token, parent, name)
            ok("""{"ino":$ino}""")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/create")
    fun create(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ) =
        try {
            val ino = fs.create(token, parent, name)
            ok("""{"ino":$ino}""")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/mkdir")
    fun mkdir(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ) =
        try {
            val ino = fs.mkdir(token, parent, name)
            ok("""{"ino":$ino}""")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/unlink")
    fun unlink(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ) =
        try {
            fs.unlink(token, parent, name)
            ok("null")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/rmdir")
    fun rmdir(
        @RequestParam token: String?,
        @RequestParam parent: Long,
        @RequestParam name: String
    ) =
        try {
            fs.rmdir(token, parent, name)
            ok("null")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/read")
    fun read(
        @RequestParam token: String?,
        @RequestParam ino: Long,
        @RequestParam offset: Int,
        @RequestParam len: Int
    ) =
        try {
            val bytes = fs.read(token, ino, offset, len)
            val b64 = fs.b64encode(bytes)
            // ВАЖНО: поле внутри data должно называться "data" (модуль ищет именно его)
            ok("""{"data":"$b64","read":${bytes.size}}""")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/write")
    fun write(
        @RequestParam token: String?,
        @RequestParam ino: Long,
        @RequestParam offset: Int,
        @RequestParam data: String  // ВАЖНО: param name "data" (модуль шлёт именно его)
    ) =
        try {
            val n = fs.write(token, ino, offset, fs.b64decode(data))
            ok("""{"written":$n}""")
        } catch (e: FsException) {
            err(e)
        }

    @GetMapping("/link")
    fun link(
        @RequestParam token: String?,
        @RequestParam oldIno: Long,
        @RequestParam newParent: Long,
        @RequestParam newName: String
    ) =
        try {
            fs.link(token, oldIno, newParent, newName)
            ok("null")
        } catch (e: FsException) {
            err(e)
        }
}
