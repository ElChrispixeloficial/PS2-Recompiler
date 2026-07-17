package com.chrispixel.ps2recompiler

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.View
import android.widget.TextView
import android.widget.LinearLayout
import android.graphics.Color
import android.widget.Button
import androidx.appcompat.app.AppCompatActivity

class TestActivity : AppCompatActivity() {

    companion object {
        init { System.loadLibrary("ps2recomp") }
    }

    private external fun nativeTestInit()
    private external fun nativeTestMips(): String
    private external fun nativeTestSpu2(): String
    private external fun nativeTestVulkan(): String
    private external fun nativeTestGs(): String
    private external fun nativeTestRun(): String

    private lateinit var tvResults: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.BLACK)
            setPadding(32, 32, 32, 32)
        }

        val title = TextView(this).apply {
            text = "PR2 - Test Suite"
            textSize = 24f
            setTextColor(Color.WHITE)
            setPadding(0, 16, 0, 24)
        }
        layout.addView(title)

        tvResults = TextView(this).apply {
            text = "Presiona los botones para probar..."
            textSize = 14f
            setTextColor(Color.GREEN)
            setPadding(0, 0, 0, 16)
        }
        layout.addView(tvResults)

        fun addButton(label: String, action: () -> String) {
            val btn = Button(this).apply {
                text = label
                setBackgroundColor(Color.parseColor("#7B2FBE"))
                setTextColor(Color.WHITE)
                setOnClickListener {
                    val result = action()
                    tvResults.append("\n\n$label:\n$result")
                }
            }
            layout.addView(btn)
        }

        addButton("Inicializar Sistema") {
            nativeTestInit()
            "✅ Sistema inicializado"
        }

        addButton("Probar MIPS→ARM64") {
            nativeTestMips()
        }

        addButton("Probar SPU2 Audio") {
            nativeTestSpu2()
        }

        addButton("Probar Vulkan") {
            nativeTestVulkan()
        }

        addButton("Probar GS Gráficos") {
            nativeTestGs()
        }

        addButton("Ejecutar Todo") {
            nativeTestRun()
        }

        setContentView(layout)
    }
}
