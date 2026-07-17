package com.chrispixel.ps2recompiler
import android.view.LayoutInflater; import android.view.View; import android.view.ViewGroup
import android.widget.TextView
import androidx.recyclerview.widget.DiffUtil; import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import java.text.SimpleDateFormat; import java.util.Date; import java.util.Locale

class GameLibraryAdapter(
    private val onPlay: (GameEntry) -> Unit,
    private val onLongPress: (GameEntry, View) -> Unit
) : ListAdapter<GameEntry, GameLibraryAdapter.VH>(object : DiffUtil.ItemCallback<GameEntry>() {
    override fun areItemsTheSame(a: GameEntry, b: GameEntry) = a.isoPath == b.isoPath
    override fun areContentsTheSame(a: GameEntry, b: GameEntry) = a == b
}) {
    private val fmt = SimpleDateFormat("dd/MM/yyyy", Locale.getDefault())

    inner class VH(v: View) : RecyclerView.ViewHolder(v) {
        val card: MaterialCardView  = v.findViewById(R.id.card_game)
        val tvTitle: TextView       = v.findViewById(R.id.tv_game_title)
        val tvId: TextView          = v.findViewById(R.id.tv_game_id)
        val tvLast: TextView        = v.findViewById(R.id.tv_last_played)
        val btnPlay: MaterialButton = v.findViewById(R.id.btn_play)
    }

    override fun onCreateViewHolder(p: ViewGroup, t: Int) =
        VH(LayoutInflater.from(p.context).inflate(R.layout.item_game, p, false))

    override fun onBindViewHolder(h: VH, pos: Int) {
        val g = getItem(pos)
        h.tvTitle.text = g.title
        h.tvId.text    = g.id
        h.tvLast.text  = if (g.lastPlayedAt == 0L) h.itemView.context.getString(R.string.never_played)
                         else h.itemView.context.getString(R.string.last_played, fmt.format(Date(g.lastPlayedAt)))
        h.btnPlay.setOnClickListener { onPlay(g) }
        h.card.setOnClickListener { onPlay(g) }
        h.card.setOnLongClickListener { onLongPress(g, it); true }
    }
}
