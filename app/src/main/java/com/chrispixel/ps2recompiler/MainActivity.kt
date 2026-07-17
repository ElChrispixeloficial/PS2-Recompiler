package com.chrispixel.ps2recompiler

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.View
import android.view.animation.AnimationUtils
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityOptionsCompat
import androidx.core.content.ContextCompat
import androidx.recyclerview.widget.LinearLayoutManager
import com.chrispixel.ps2recompiler.databinding.ActivityMainBinding
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.snackbar.Snackbar
import java.io.File

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var adapter: GameLibraryAdapter
    private val games = mutableListOf<GameEntry>()

    // Configuración del recompilador
    private var recompilerConfig = RecompilerConfig()

    data class RecompilerConfig(
        var eeEnabled: Boolean = true,
        var iopEnabled: Boolean = false,
        var vuEnabled: Boolean = false,
        var resolutionScale: Int = 1,
        var fpsLimit: Int = 60,
        var audioEnabled: Boolean = true
    )

    private val isoPicker = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        uri?.let { handleIsoUri(it) }
    }

    // Selector de BIOS
    private val biosPicker = registerForActivityResult(ActivityResultContracts.GetContent()) { uri ->
        uri?.let { handleBiosUri(it) }
    }

    private val permLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants.values.all { it }) isoPicker.launch("*/*")
        else Snackbar.make(binding.root, "Permiso necesario", Snackbar.LENGTH_LONG).show()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        setSupportActionBar(binding.toolbar)

        // Animación de entrada
        window.enterTransition = AnimationUtils.loadAnimation(this, android.R.anim.fade_in).let {
            android.transition.Fade().apply { duration = 300 }
        }

        binding.toolbar.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.action_settings -> showConfigDialog()
                R.id.action_about -> showInfo(getString(R.string.about_title),
                    getString(R.string.about_body))
            }
            true
        }

        adapter = GameLibraryAdapter(
            onPlay = { launchGame(it) },
            onLongPress = { game, _ ->
                MaterialAlertDialogBuilder(this)
                    .setTitle(game.title)
                    .setItems(arrayOf("Jugar", "Eliminar")) { _, w ->
                        if (w == 0) launchGame(game)
                        else {
                            GameLibrary.remove(this, game.isoPath)
                            loadLibrary()
                        }
                    }.show()
            }
        )

        binding.recyclerGames.layoutManager = LinearLayoutManager(this)
        binding.recyclerGames.adapter = adapter
        binding.fabAddGame.setOnClickListener { pickIso() }

        // Botón TEST
        binding.btnTest.visibility = View.VISIBLE
        binding.btnTest.setOnClickListener {
            startActivity(Intent(this, TestActivity::class.java),
                ActivityOptionsCompat.makeSceneTransitionAnimation(this).toBundle())
        }

        // Lógica de selección de BIOS
        binding.btnBiosSelect.visibility = View.VISIBLE
        binding.btnBiosSelect.setOnClickListener {
            biosPicker.launch("*/*")
        }
        
        // Cargar estado de la BIOS
        val biosPath = getSharedPreferences("ps2_prefs", MODE_PRIVATE).getString("bios_path", null)
        if (biosPath != null) {
            binding.tvBiosStatus.text = "BIOS Oficial cargada ✓"
            binding.tvBiosStatus.setTextColor(ContextCompat.getColor(this, R.color.bios_ok))
        } else {
            binding.tvBiosStatus.text = "Selecciona BIOS oficial (scph-XXXX.bin)"
            binding.tvBiosStatus.setTextColor(ContextCompat.getColor(this, R.color.bios_missing))
        }

        loadLibrary()
        intent?.data?.let { handleIsoUri(it) }
    }

    private fun handleBiosUri(uri: Uri) {
        val path = resolveUri(uri, "bios.bin") ?: run {
            Snackbar.make(binding.root, "Error al cargar BIOS", Snackbar.LENGTH_LONG).show()
            return
        }
        // Guardar la ruta de la BIOS en preferencias
        getSharedPreferences("ps2_prefs", MODE_PRIVATE).edit().putString("bios_path", path).apply()
        binding.tvBiosStatus.text = "BIOS Oficial cargada ✓"
        binding.tvBiosStatus.setTextColor(ContextCompat.getColor(this, R.color.bios_ok))
        Snackbar.make(binding.root, "✅ BIOS cargada correctamente", Snackbar.LENGTH_SHORT).show()
    }

    override fun onResume() {
        super.onResume()
        loadLibrary()
    }

    private fun showConfigDialog() {
        val items = arrayOf(
            "EE Recompiler: ${if (recompilerConfig.eeEnabled) "ON" else "OFF"}",
            "IOP: ${if (recompilerConfig.iopEnabled) "ON" else "OFF"}",
            "VU0/VU1: ${if (recompilerConfig.vuEnabled) "ON" else "OFF"}",
            "Resolución: ${recompilerConfig.resolutionScale}x",
            "Límite FPS: ${recompilerConfig.fpsLimit}",
            "Audio: ${if (recompilerConfig.audioEnabled) "ON" else "OFF"}"
        )
        val checked = booleanArrayOf(
            recompilerConfig.eeEnabled,
            recompilerConfig.iopEnabled,
            recompilerConfig.vuEnabled,
            false, false, false
        )

        MaterialAlertDialogBuilder(this)
            .setTitle("Configuración del Recompilador")
            .setMultiChoiceItems(items, checked) { _, which, isChecked ->
                when (which) {
                    0 -> recompilerConfig.eeEnabled = isChecked
                    1 -> recompilerConfig.iopEnabled = isChecked
                    2 -> recompilerConfig.vuEnabled = isChecked
                }
            }
            .setPositiveButton("OK", null)
            .setNeutralButton("Resolución") { _, _ ->
                MaterialAlertDialogBuilder(this)
                    .setTitle("Escala de resolución")
                    .setSingleChoiceItems(arrayOf("1x (PS2)", "2x", "3x", "4x (HD)"), recompilerConfig.resolutionScale - 1) { _, w ->
                        recompilerConfig.resolutionScale = w + 1
                    }
                    .setPositiveButton("OK", null)
                    .show()
            }
            .show()
    }

    private fun pickIso() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            isoPicker.launch("*/*")
            return
        }
        val p = Manifest.permission.READ_EXTERNAL_STORAGE
        if (ContextCompat.checkSelfPermission(this, p) == PackageManager.PERMISSION_GRANTED)
            isoPicker.launch("*/*")
        else
            permLauncher.launch(arrayOf(p))
    }

    private fun handleIsoUri(uri: Uri) {
        val path = resolveUri(uri) ?: run {
            Snackbar.make(binding.root, getString(R.string.error_load_iso), Snackbar.LENGTH_LONG).show()
            return
        }
        val name = displayName(uri) ?: File(path).nameWithoutExtension
        val entry = GameEntry(name.take(12).uppercase(), name, path)
        GameLibrary.add(this, entry)
        loadLibrary()
        Snackbar.make(binding.root, "\"${entry.title}\" añadido", Snackbar.LENGTH_SHORT)
            .setAction("Jugar") { launchGame(entry) }.show()
    }

    private fun resolveUri(uri: Uri, fileName: String = "game.iso"): String? = when (uri.scheme) {
        "file" -> uri.path
        "content" -> try {
            val f = File(cacheDir, fileName)
            contentResolver.openInputStream(uri)?.use { i ->
                f.outputStream().use { o -> i.copyTo(o) }
            }
            f.absolutePath
        } catch (e: Exception) { null }
        else -> null
    }

    private fun displayName(uri: Uri) = contentResolver.query(uri, null, null, null, null)?.use { c ->
        c.moveToFirst()
        c.getString(c.getColumnIndex(OpenableColumns.DISPLAY_NAME))
    }

    private fun loadLibrary() {
        games.clear()
        games.addAll(GameLibrary.loadAll(this))
        adapter.submitList(games.toList())
        val has = games.isNotEmpty()
        binding.layoutEmpty.visibility = if (has) View.GONE else View.VISIBLE
        binding.recyclerGames.visibility = if (has) View.VISIBLE else View.GONE
        binding.tvGameCount.text = "${games.size} ${if (games.size == 1) "juego" else "juegos"}"
    }

    private fun launchGame(game: GameEntry) {
        GameLibrary.updateLastPlayed(this, game.isoPath)
        
        // Pasar la ruta de la BIOS a RuntimeActivity
        val biosPath = getSharedPreferences("ps2_prefs", MODE_PRIVATE).getString("bios_path", "")
        
        val intent = Intent(this, RuntimeActivity::class.java).apply {
            putExtra(RuntimeActivity.EXTRA_ISO_PATH, game.isoPath)
            putExtra(RuntimeActivity.EXTRA_GAME_TITLE, game.title)
            putExtra("bios_path", biosPath) // <--- Pasar la BIOS
        }
        startActivity(intent,
            ActivityOptionsCompat.makeSceneTransitionAnimation(this).toBundle())
    }

    private fun showInfo(title: String, msg: String) {
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(msg)
            .setPositiveButton("Cerrar", null)
            .show()
    }
}