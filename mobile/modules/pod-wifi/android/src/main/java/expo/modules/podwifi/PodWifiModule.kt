package expo.modules.podwifi

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import expo.modules.kotlin.Promise
import expo.modules.kotlin.exception.CodedException
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition

// Joins the pod's own AP the way Android 10+ wants a local-only device joined:
// WifiNetworkSpecifier shows the system's one-tap "connect to this device"
// prompt, and the network it hands back is ours alone - the phone keeps mobile
// data as its default route, so nothing else on the phone loses internet.
//
// That also means our own sockets would still go out over mobile data unless
// told otherwise, so the whole process is bound to the pod's network while
// connected. fetch() and expo-file-system downloads then reach 4.3.2.1. The
// flip side: nothing in this app reaches the internet until disconnect() -
// a sync pulls everything first, disconnects, then uploads.
class PodWifiModule : Module() {
  private var callback: ConnectivityManager.NetworkCallback? = null

  private val cm: ConnectivityManager
    get() = appContext.reactContext?.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
      ?: throw CodedException("ERR_NO_CONTEXT", "No Android context", null)

  override fun definition() = ModuleDefinition {
    Name("PodWifi")

    // The pod dropped off (out of range, or it went to sleep). JS hears it so
    // a sync in progress can stop and resume later instead of hanging.
    Events("onLost")

    AsyncFunction("connect") { ssid: String, passphrase: String, timeoutMs: Int, promise: Promise ->
      if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
        promise.reject(CodedException("ERR_OLD_ANDROID", "Needs Android 10 or newer", null))
        return@AsyncFunction
      }
      release()

      val specifier = WifiNetworkSpecifier.Builder()
        .setSsid(ssid)
        .setWpa2Passphrase(passphrase)
        .build()
      val request = NetworkRequest.Builder()
        .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
        // The pod has no internet. Without this Android would wait for a
        // network that validates, which this one never will.
        .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
        .setNetworkSpecifier(specifier)
        .build()

      // onAvailable can fire again after a brief drop; the promise settles once.
      var settled = false
      val cb = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
          cm.bindProcessToNetwork(network)
          if (!settled) { settled = true; promise.resolve(null) }
        }

        // Also the timeout path, and the user tapping "cancel" on the prompt.
        override fun onUnavailable() {
          callback = null
          if (!settled) {
            settled = true
            promise.reject(CodedException("ERR_UNAVAILABLE", "Gauge not found, or the prompt was declined", null))
          }
        }

        override fun onLost(network: Network) {
          cm.bindProcessToNetwork(null)
          sendEvent("onLost", mapOf<String, Any>())
        }
      }
      callback = cb
      cm.requestNetwork(request, cb, timeoutMs)
    }

    // Unbinds first, so the app's traffic goes back to the default network
    // even if the pod's network is already gone.
    Function("disconnect") {
      release()
    }

    Function("isConnected") {
      callback != null && cm.boundNetworkForProcess != null
    }

    OnDestroy {
      release()
    }
  }

  private fun release() {
    val c = cm
    c.bindProcessToNetwork(null)
    callback?.let { runCatching { c.unregisterNetworkCallback(it) } }
    callback = null
  }
}
