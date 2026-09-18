package com.ayana.engine

import android.view.Surface
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry

// The whole Kotlin side: hand the engine a Surface to draw into and a texture id for Flutter's
// `Texture` widget. Everything else (eng_* through the render queue) goes straight from Dart to
// libayana.so over FFI, so there is no per-frame JNI and no method channel traffic.
class AyanaPlugin : FlutterPlugin, MethodChannel.MethodCallHandler {
    private lateinit var channel: MethodChannel
    private var textures: TextureRegistry? = null
    private var entry: TextureRegistry.SurfaceTextureEntry? = null
    private var surface: Surface? = null

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        textures = binding.textureRegistry
        channel = MethodChannel(binding.binaryMessenger, "ayana")
        channel.setMethodCallHandler(this)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        dispose()
        textures = null
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            // create(width, height, dpr) -> textureId. Starts the render thread and eng_init.
            "create" -> {
                val w = call.argument<Int>("width") ?: 1
                val h = call.argument<Int>("height") ?: 1
                val dpr = (call.argument<Double>("dpr") ?: 1.0).toFloat()
                if (entry != null) { result.error("busy", "a texture already exists", null); return }
                val e = textures!!.createSurfaceTexture()
                e.surfaceTexture().setDefaultBufferSize(w, h)
                val s = Surface(e.surfaceTexture())
                entry = e
                surface = s
                System.loadLibrary("ayana")
                nativeAttach(s, w, h, dpr)
                result.success(e.id())
            }
            "resize" -> {
                val w = call.argument<Int>("width") ?: 1
                val h = call.argument<Int>("height") ?: 1
                val dpr = (call.argument<Double>("dpr") ?: 1.0).toFloat()
                entry?.surfaceTexture()?.setDefaultBufferSize(w, h)
                nativeResize(w, h, dpr)
                result.success(null)
            }
            "dispose" -> { dispose(); result.success(null) }
            else -> result.notImplemented()
        }
    }

    private fun dispose() {
        if (entry == null) return
        nativeDetach()        // blocks until the render thread let the Surface go
        nativeStop()
        surface?.release(); surface = null
        entry?.release(); entry = null
    }

    companion object {
        @JvmStatic external fun nativeAttach(surface: Surface, width: Int, height: Int, dpr: Float)
        @JvmStatic external fun nativeResize(width: Int, height: Int, dpr: Float)
        @JvmStatic external fun nativeDetach()
        @JvmStatic external fun nativeStop()
    }
}
