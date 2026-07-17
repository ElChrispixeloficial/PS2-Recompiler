package com.chrispixel.ps2recompiler

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.SurfaceHolder
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.chrispixel.ps2recompiler.databinding.ActivityRuntimeBinding
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.*

class RuntimeActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_ISO_PATH   = "iso_path"
        const val EXTRA_GAME_TITLE = "game_title"
        init { System.loadLibrary("ps2recomp") }
    }

    private external fun nativeLoadISO(path: String): Boolean
    private external fun nativeLoadBIOS(path: String): Boolean
    private external fun nativeSurfaceCreated(surface: Any)
    private external fun nativeSurfaceChanged(surface: Any, w: Int, h: Int)
    private external fun nativeSurfaceDestroyed()
    private external fun nativeResume()
    private external fun nativePause()
    private external fun nativeReset()
    private external fun nativeShutdown()
    private external fun nativeGetFps(): Int
    private external fun nativeGetDebugInfo(): String
    private external fun nativeIsAlertActive(): Boolean

    private lateinit var binding: ActivityRuntimeBinding
    private var isoPath   = ""
    private var gameTitle = ""
    private var isPaused  = false
    private var isLoaded  = false
    private var hudJob: Job? = null

    // Vistas de depuración creadas por código
    private lateinit var debugTextView: TextView
    private lateinit var btnCopyLogs: Button
    private val logHistory = StringBuilder()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityRuntimeBinding.inflate(layoutInflater)
        setContentView(binding.root)

        isoPath   = intent.getStringExtra(EXTRA_ISO_PATH)   ?: ""
        gameTitle = intent.getStringExtra(EXTRA_GAME_TITLE) ?: "Juego"
        if (isoPath.isEmpty()) { finish(); return }
        
        immersive()
        binding.tvGameTitle.text  = gameTitle
        binding.tvPausedGame.text = gameTitle
        
        // --- CREAR VISTA DE DEBUG POR CÓDIGO (NO TOCAS XML) ---
        setupDebugOverlay()
        
        binding.surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder)  { nativeSurfaceCreated(h.surface); if (!isLoaded) load() }
            override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, ht: Int) { nativeSurfaceChanged(h.surface, w, ht) }
            override fun surfaceDestroyed(h: SurfaceHolder){ nativeSurfaceDestroyed() }
        })
        binding.surfaceView.setOnClickListener { toggleHud() }
        binding.btnBack.setOnClickListener    { togglePause() }
        binding.btnPause.setOnClickListener   { togglePause() }
        binding.btnResume.setOnClickListener  { resume() }
        binding.btnReset.setOnClickListener   { 
            hidePauseOv()
            nativeReset()
            isLoaded = false
            logHistory.clear()
            load() 
            isPaused = false 
        }
        binding.btnQuit.setOnClickListener    { MaterialAlertDialogBuilder(this)
            .setTitle("¿Salir?").setMessage("Se perderá el progreso no guardado.")
            .setPositiveButton("Salir") { _, _ -> nativeShutdown(); finish() }
            .setNegativeButton("Cancelar", null).show() }
    }

    private fun setupDebugOverlay() {
        val rootLayout = binding.root as? FrameLayout
            ?: (binding.root as? ViewGroup)?.let { 
                for (i in 0 until it.childCount) {
                    if (it.getChildAt(i) is FrameLayout) return@let it.getChildAt(i) as FrameLayout
                }
                null
            } ?: return

        debugTextView = TextView(this).apply {
            text = "Iniciando depuración..."
            setTextColor(Color.GREEN)
            textSize = 12f
            typeface = Typeface.MONOSPACE
            setPadding(32, 100, 32, 32)
            setTextIsSelectable(true)
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        }
        rootLayout.addView(debugTextView)

        btnCopyLogs = Button(this).apply {
            text = "Copiar Logs"
            setBackgroundColor(Color.parseColor("#AA000000"))
            setTextColor(Color.WHITE)
            alpha = 0.8f
            val params = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT
            )
            params.gravity = Gravity.BOTTOM or Gravity.START
            params.marginStart = 24
            params.bottomMargin = 100
            layoutParams = params
            setOnClickListener {
                val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                val clip = ClipData.newPlainText("Logs PS2", logHistory.toString())
                clipboard.setPrimaryClip(clip)
                Toast.makeText(this@RuntimeActivity, "Logs copiados al portapapeles", Toast.LENGTH_SHORT).show()
            }
        }
        rootLayout.addView(btnCopyLogs)
    }

    override fun onResume()  { super.onResume();  if (isLoaded && isPaused) resume() }
    override fun onPause()   { super.onPause();   if (isLoaded && !isPaused) { isPaused = true; nativePause() } }
    override fun onDestroy() { super.onDestroy(); nativeShutdown() }
    @Deprecated("Deprecated in Java")
    override fun onBackPressed() { if (isLoaded) togglePause() else super.onBackPressed() }

    private fun load() {
        binding.layoutLoading.visibility = View.VISIBLE
        binding.tvLoadingStatus.text = getString(R.string.game_loading)
        binding.tvLoadingDetail.text = isoPath.substringAfterLast('/')
        
        lifecycleScope.launch(Dispatchers.IO) {
            // 1. Cargar la BIOS pasada desde MainActivity
            val biosPath = intent.getStringExtra("bios_path")
            var biosLoaded = false
            if (!biosPath.isNullOrEmpty()) {
                biosLoaded = nativeLoadBIOS(biosPath)
            }
            
            // 2. Cargar el juego
            val ok = nativeLoadISO(isoPath)
            
            withContext(Dispatchers.Main) {
                if (ok) {
                    binding.tvLoadingStatus.text = getString(R.string.game_compiling)
                    delay(300)
                    binding.layoutLoading.visibility = View.GONE
                    isLoaded = true; nativeResume(); fpsLoop(); showHud()
                    if (!biosLoaded) {
                        Snackbar.make(binding.root, "⚠️ Falta BIOS oficial. Modo HLE activado.", Snackbar.LENGTH_LONG).show()
                    } else {
                        Snackbar.make(binding.root, "✅ BIOS oficial cargada. Arranque orgánico.", Snackbar.LENGTH_SHORT).show()
                    }
                } else {
                    binding.progressLoading.visibility = View.GONE
                    binding.tvLoadingStatus.text = getString(R.string.error_load_iso)
                    Snackbar.make(binding.root, R.string.error_load_iso, Snackbar.LENGTH_INDEFINITE)
                        .setAction("Salir") { finish() }.show()
                }
            }
        }
    }

    private fun fpsLoop() = lifecycleScope.launch {
        while (isLoaded) { 
            delay(250)
            val debugInfo = nativeGetDebugInfo()
            
            if (logHistory.isEmpty() || !logHistory.endsWith(debugInfo)) {
                logHistory.append(debugInfo).append("\n----------------\n")
            }
            
            debugTextView.text = debugInfo
            
            if (nativeIsAlertActive()) {
                debugTextView.setTextColor(Color.RED)
            } else {
                debugTextView.setTextColor(Color.GREEN)
            }
        }
    }

    private fun toggleHud()   { if (binding.hudTop.visibility == View.VISIBLE) hideHud() else showHud() }
    private fun showHud()     { binding.hudTop.visibility = View.VISIBLE; binding.hudTop.animate().alpha(1f).setDuration(200).start()
        hudJob?.cancel(); hudJob = lifecycleScope.launch { delay(3000); hideHud() } }
    private fun hideHud()     { hudJob?.cancel(); binding.hudTop.animate().alpha(0f).setDuration(400)
        .withEndAction { binding.hudTop.visibility = View.GONE }.start() }

    private fun togglePause() { if (isPaused) resume() else pause() }
    private fun pause()       { isPaused = true; nativePause(); showPauseOv(); showHud() }
    private fun resume()      { isPaused = false; hidePauseOv(); nativeResume(); showHud() }
    private fun showPauseOv() { binding.overlayPause.visibility = View.VISIBLE; binding.overlayPause.animate().alpha(1f).setDuration(250).start() }
    private fun hidePauseOv() { binding.overlayPause.animate().alpha(0f).setDuration(200)
        .withEndAction { binding.overlayPause.visibility = View.GONE }.start() }

    private fun immersive() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION)
        }
    }
}