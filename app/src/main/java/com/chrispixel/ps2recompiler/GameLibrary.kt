package com.chrispixel.ps2recompiler
import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject

object GameLibrary {
    private const val PREFS_NAME = "game_library"
    private const val KEY_GAMES  = "games"
    private fun prefs(ctx: Context): SharedPreferences =
        ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    fun loadAll(ctx: Context): List<GameEntry> {
        val arr = JSONArray(prefs(ctx).getString(KEY_GAMES, "[]") ?: "[]")
        return (0 until arr.length()).map { i ->
            val o = arr.getJSONObject(i)
            GameEntry(o.getString("id"), o.getString("title"), o.getString("isoPath"),
                o.getLong("addedAt"), o.optLong("lastPlayedAt", 0L))
        }
    }
    fun save(ctx: Context, games: List<GameEntry>) {
        val arr = JSONArray()
        games.forEach { g -> arr.put(JSONObject().apply {
            put("id", g.id); put("title", g.title); put("isoPath", g.isoPath)
            put("addedAt", g.addedAt); put("lastPlayedAt", g.lastPlayedAt)
        })}
        prefs(ctx).edit().putString(KEY_GAMES, arr.toString()).apply()
    }
    fun add(ctx: Context, entry: GameEntry) {
        val list = loadAll(ctx).toMutableList()
        list.removeAll { it.isoPath == entry.isoPath }
        list.add(0, entry); save(ctx, list)
    }
    fun remove(ctx: Context, isoPath: String) {
        save(ctx, loadAll(ctx).toMutableList().also { it.removeAll { e -> e.isoPath == isoPath } })
    }
    fun updateLastPlayed(ctx: Context, isoPath: String) {
        val list = loadAll(ctx).toMutableList()
        val idx = list.indexOfFirst { it.isoPath == isoPath }
        if (idx >= 0) { list[idx] = list[idx].copy(lastPlayedAt = System.currentTimeMillis()); save(ctx, list) }
    }
}
