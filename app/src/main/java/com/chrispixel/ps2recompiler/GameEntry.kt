package com.chrispixel.ps2recompiler
data class GameEntry(
    val id: String,
    val title: String,
    val isoPath: String,
    val addedAt: Long = System.currentTimeMillis(),
    val lastPlayedAt: Long = 0L
)
