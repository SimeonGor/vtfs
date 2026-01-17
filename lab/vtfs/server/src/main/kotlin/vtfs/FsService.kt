package vtfs

import java.util.Base64
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicLong

enum class NodeType { FILE, DIR }

data class Inode(
    val id: Long,
    val type: NodeType,
    var mode: Int = 511,   // 0777
    var nlink: Int = 1,
    var size: Int = 0
)

class FsException(val code: Int, message: String) : RuntimeException(message)

/**
 * Простейшая in-memory ФС:
 * - inode хранится в inodes
 * - директория: dirEntries[dirIno] = name -> childIno
 * - файл: fileData[ino] = ByteArray
 *
 * Hardlink: несколько имён в каталогах могут ссылаться на один и тот же ino (только FILE).
 */
class FsService(
    private val tokenValue: String = "TODO_TOKEN"
) {
    companion object {
        const val OK = 0

        // коды ошибок "в стиле errno" (можно адаптировать под ваши тесты)
        const val E_PERM = 1
        const val E_NOENT = 2
        const val E_EXIST = 17
        const val E_NOTDIR = 20
        const val E_ISDIR = 21
        const val E_NOTEMPTY = 39
    }

    private val inodes = ConcurrentHashMap<Long, Inode>()
    private val dirEntries = ConcurrentHashMap<Long, ConcurrentHashMap<String, Long>>() // only for DIR
    private val fileData = ConcurrentHashMap<Long, ByteArray>() // only for FILE

    private val nextIno = AtomicLong(2000)

    // корень
    val rootIno: Long = 1000

    init {
        val root = Inode(id = rootIno, type = NodeType.DIR, mode = 511, nlink = 1)
        inodes[rootIno] = root
        dirEntries[rootIno] = ConcurrentHashMap()
    }

    private fun auth(token: String?) {
        if (token == null || token != tokenValue) throw FsException(E_PERM, "bad token")
    }

    fun b64encode(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)
    fun b64decode(s: String): ByteArray = Base64.getDecoder().decode(s)

    @Synchronized
    fun list(token: String?, dir: Long): Map<String, Long> {
        auth(token)
        val d = inodes[dir] ?: throw FsException(E_NOENT, "dir not found")
        if (d.type != NodeType.DIR) throw FsException(E_NOTDIR, "not a dir")
        return dirEntries[dir]?.toMap() ?: emptyMap()
    }

    @Synchronized
    fun lookup(token: String?, parent: Long, name: String): Long {
        auth(token)
        val p = inodes[parent] ?: throw FsException(E_NOENT, "parent not found")
        if (p.type != NodeType.DIR) throw FsException(E_NOTDIR, "parent not dir")
        return dirEntries[parent]?.get(name) ?: throw FsException(E_NOENT, "no such entry")
    }

    @Synchronized
    fun create(token: String?, parent: Long, name: String): Long {
        auth(token)
        val p = inodes[parent] ?: throw FsException(E_NOENT, "parent not found")
        if (p.type != NodeType.DIR) throw FsException(E_NOTDIR, "parent not dir")

        val entries = dirEntries[parent]!!
        if (entries.containsKey(name)) throw FsException(E_EXIST, "already exists")

        val ino = nextIno.getAndIncrement()
        inodes[ino] = Inode(id = ino, type = NodeType.FILE, mode = 511, nlink = 1, size = 0)
        fileData[ino] = ByteArray(0)
        entries[name] = ino
        return ino
    }

    @Synchronized
    fun mkdir(token: String?, parent: Long, name: String): Long {
        auth(token)
        val p = inodes[parent] ?: throw FsException(E_NOENT, "parent not found")
        if (p.type != NodeType.DIR) throw FsException(E_NOTDIR, "parent not dir")

        val entries = dirEntries[parent]!!
        if (entries.containsKey(name)) throw FsException(E_EXIST, "already exists")

        val ino = nextIno.getAndIncrement()
        inodes[ino] = Inode(id = ino, type = NodeType.DIR, mode = 511, nlink = 1)
        dirEntries[ino] = ConcurrentHashMap()
        entries[name] = ino
        return ino
    }

    @Synchronized
    fun unlink(token: String?, parent: Long, name: String) {
        auth(token)

        val p = inodes[parent] ?: throw FsException(E_NOENT, "parent not found")
        if (p.type != NodeType.DIR) throw FsException(E_NOTDIR, "parent not dir")

        val entries = dirEntries[parent]!!
        val ino = entries[name] ?: throw FsException(E_NOENT, "no such entry")
        val node = inodes[ino] ?: throw FsException(E_NOENT, "inode missing")

        if (node.type == NodeType.DIR) throw FsException(E_ISDIR, "is a dir")

        // удалить запись из каталога
        entries.remove(name)

        // hardlink semantics: nlink--
        node.nlink -= 1
        if (node.nlink <= 0) {
            inodes.remove(ino)
            fileData.remove(ino)
        }
    }

    @Synchronized
    fun rmdir(token: String?, parent: Long, name: String) {
        auth(token)

        val p = inodes[parent] ?: throw FsException(E_NOENT, "parent not found")
        if (p.type != NodeType.DIR) throw FsException(E_NOTDIR, "parent not dir")

        val entries = dirEntries[parent]!!
        val ino = entries[name] ?: throw FsException(E_NOENT, "no such entry")
        val node = inodes[ino] ?: throw FsException(E_NOENT, "inode missing")

        if (node.type != NodeType.DIR) throw FsException(E_NOTDIR, "not a dir")

        val childEntries = dirEntries[ino]!!
        if (childEntries.isNotEmpty()) throw FsException(E_NOTEMPTY, "dir not empty")

        entries.remove(name)
        dirEntries.remove(ino)
        inodes.remove(ino)
    }

    @Synchronized
    fun read(token: String?, ino: Long, offset: Int, len: Int): ByteArray {
        auth(token)

        val node = inodes[ino] ?: throw FsException(E_NOENT, "not found")
        if (node.type != NodeType.FILE) throw FsException(E_ISDIR, "is a dir")

        val data = fileData[ino] ?: ByteArray(0)
        if (offset >= data.size) return ByteArray(0)

        val end = minOf(data.size, offset + len)
        return data.copyOfRange(offset, end)
    }

    @Synchronized
    fun write(token: String?, ino: Long, offset: Int, chunk: ByteArray): Int {
        auth(token)

        val node = inodes[ino] ?: throw FsException(E_NOENT, "not found")
        if (node.type != NodeType.FILE) throw FsException(E_ISDIR, "is a dir")

        val old = fileData[ino] ?: ByteArray(0)
        val needSize = maxOf(old.size, offset + chunk.size)
        val newArr = ByteArray(needSize)

        // copy old
        System.arraycopy(old, 0, newArr, 0, old.size)
        // write new data at offset
        System.arraycopy(chunk, 0, newArr, offset, chunk.size)

        fileData[ino] = newArr
        node.size = newArr.size
        return chunk.size
    }

    /**
     * Hardlink: добавить новое имя в каталоге newParent на существующий inode oldIno (только FILE).
     */
    @Synchronized
    fun link(token: String?, oldIno: Long, newParent: Long, newName: String) {
        auth(token)

        val target = inodes[oldIno] ?: throw FsException(E_NOENT, "old inode not found")
        if (target.type != NodeType.FILE) throw FsException(E_PERM, "hardlink only for files")

        val p = inodes[newParent] ?: throw FsException(E_NOENT, "parent not found")
        if (p.type != NodeType.DIR) throw FsException(E_NOTDIR, "parent not dir")

        val entries = dirEntries[newParent] ?: throw FsException(E_NOTDIR, "parent entries missing")
        if (entries.containsKey(newName)) throw FsException(E_EXIST, "already exists")

        entries[newName] = oldIno
        target.nlink += 1
    }
}
