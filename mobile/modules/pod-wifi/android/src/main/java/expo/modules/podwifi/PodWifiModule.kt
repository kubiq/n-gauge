package expo.modules.podwifi

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.ConnectivityManager
import android.net.MacAddress
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import androidx.core.content.ContextCompat
import expo.modules.kotlin.Promise
import expo.modules.kotlin.exception.CodedException
import expo.modules.kotlin.modules.Module
import expo.modules.kotlin.modules.ModuleDefinition
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.concurrent.thread

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
  // The request for the pod's network (with the system prompt), the watch
  // for one the phone joined by itself, and the network in use.
  @Volatile private var requested: ConnectivityManager.NetworkCallback? = null
  @Volatile private var watching: ConnectivityManager.NetworkCallback? = null
  @Volatile private var bound: Network? = null
  private var wifiReceiver: BroadcastReceiver? = null

  private val cm: ConnectivityManager
    get() = appContext.reactContext?.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager
      ?: throw CodedException("ERR_NO_CONTEXT", "No Android context", null)

  override fun definition() = ModuleDefinition {
    Name("PodWifi")

    // The pod dropped off (out of range, or it went to sleep). JS hears it so
    // a sync in progress can stop and resume later instead of hanging.
    // onWifiOn: the phone's WiFi was switched on - after a connect that
    // failed for it being off, the app tries again (2026-10-26: switched on
    // in the panel the app opened, the chip still said "WiFi is off").
    Events("onLost", "onWifiOn")

    OnCreate {
      val r = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
          if (i.getIntExtra(WifiManager.EXTRA_WIFI_STATE, -1) == WifiManager.WIFI_STATE_ENABLED)
            sendEvent("onWifiOn", mapOf<String, Any>())
        }
      }
      // A system broadcast: delivered to a receiver that is not exported.
      runCatching {
        ContextCompat.registerReceiver(ctx.applicationContext, r,
          IntentFilter(WifiManager.WIFI_STATE_CHANGED_ACTION), ContextCompat.RECEIVER_NOT_EXPORTED)
        wifiReceiver = r
      }
    }

    AsyncFunction("connect") { ssid: String, passphrase: String, timeoutMs: Int, promise: Promise ->
      if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
        promise.reject(CodedException("ERR_OLD_ANDROID", "Needs Android 10 or newer", null))
        return@AsyncFunction
      }
      // WiFi switched off: the request would wait out its whole timeout for
      // a network it cannot find, and then fail with no word of why
      // (2026-10-26). Said at once; the app offers the switch.
      val wifi = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
      if (wifi != null && !wifi.isWifiEnabled) {
        promise.reject(CodedException("ERR_WIFI_OFF", "WiFi is off", null))
        return@AsyncFunction
      }
      release()
      val c = cm
      // One connect, settled once, by whichever way finds the pod first.
      val settled = AtomicBoolean(false)
      lateinit var asked: ConnectivityManager.NetworkCallback

      fun adopt(network: Network, viaRequest: Boolean) {
        if (!settled.compareAndSet(false, true)) return
        bound = network
        c.bindProcessToNetwork(network)
        // Found on a network the phone joined by itself: the request (and
        // its prompt) is not needed - and its network would be a second one.
        if (!viaRequest) {
          runCatching { c.unregisterNetworkCallback(asked) }
          requested = null
        }
        promise.resolve(null)
      }

      fun lost(network: Network) {
        if (network != bound) return
        bound = null
        c.bindProcessToNetwork(null)
        sendEvent("onLost", mapOf<String, Any>())
      }

      // Every WiFi network the phone is on, now or once it joins one: one
      // that answers as the pod is used as it is. Joined by hand in Android's
      // WiFi list (2026-10-22: the request found nothing in 30 s, and the
      // hand-joined network was then ignored, the connect failing anyway).
      val any = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
          if (settled.get()) return
          thread { if (isPod(network, ssid)) adopt(network, false) }
        }

        override fun onLost(network: Network) = lost(network)
      }

      // And the request, with the prompt. The pod's BSSID from a recent scan,
      // if the phone has one (it has with the beacon on): matched straight
      // from the results, and once approved, joined again without a prompt.
      val spec = WifiNetworkSpecifier.Builder()
        .setSsid(ssid)
        .setWpa2Passphrase(passphrase)
      PodBeacon.bssidOf(ctx, ssid)?.let { spec.setBssid(MacAddress.fromString(it)) }
      PodBeacon.scan(ctx)
      val request = NetworkRequest.Builder()
        .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
        // The pod has no internet. Without this Android would wait for a
        // network that validates, which this one never will.
        .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
        .setNetworkSpecifier(spec.build())
        .build()
      asked = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
          if (!settled.get()) return adopt(network, true)
          // Back after a brief drop: bound again.
          if (bound == null && requested === this) {
            bound = network
            c.bindProcessToNetwork(network)
          }
        }

        // Also the timeout path, and the user tapping "cancel" on the prompt.
        override fun onUnavailable() {
          requested = null
          if (settled.compareAndSet(false, true)) {
            release()
            promise.reject(CodedException("ERR_UNAVAILABLE", "Gauge not found, or the prompt was declined", null))
          }
        }

        override fun onLost(network: Network) = lost(network)
      }
      requested = asked
      // The watch after `asked` exists: a network found at once unregisters it.
      watching = any
      c.registerNetworkCallback(
        NetworkRequest.Builder()
          .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
          .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
          .build(),
        any,
      )
      c.requestNetwork(request, asked, timeoutMs)
    }

    // Unbinds first, so the app's traffic goes back to the default network
    // even if the pod's network is already gone.
    Function("disconnect") {
      release()
    }

    Function("isConnected") {
      bound != null && cm.boundNetworkForProcess != null
    }

    // The beacon (PodBeacon.kt): the fingerprint the gauge is beaconing, if
    // the phone's scan results from the last maxAgeMs have it.
    Function("seenBeacon") { maxAgeMs: Double ->
      PodBeacon.seen(ctx, maxAgeMs.toLong())
    }

    Function("setKnownBeacons") { fps: List<String> ->
      PodBeacon.setKnown(ctx, fps)
    }

    Function("watchBeacon") { on: Boolean, title: String, text: String ->
      PodBeacon.watch(ctx, on, title, text)
    }

    OnDestroy {
      wifiReceiver?.let { r -> runCatching { ctx.applicationContext.unregisterReceiver(r) } }
      wifiReceiver = null
      release()
    }
  }

  private val ctx: Context
    get() = appContext.reactContext ?: throw CodedException("ERR_NO_CONTEXT", "No Android context", null)

  private fun release() {
    val c = cm
    c.bindProcessToNetwork(null)
    bound = null
    for (cb in listOf(requested, watching)) cb?.let { runCatching { c.unregisterNetworkCallback(it) } }
    requested = null
    watching = null
  }

  // The pod answers on this network: its settings, with its own SSID in
  // them. Not just anything at 4.3.2.1 - that is a public address, and over a
  // network with internet something else could answer there.
  private fun isPod(network: Network, ssid: String): Boolean = runCatching {
    val conn = network.openConnection(URL("http://4.3.2.1/api/settings")) as HttpURLConnection
    conn.connectTimeout = 2000
    conn.readTimeout = 2000
    try {
      conn.responseCode == 200 && conn.inputStream.bufferedReader().readText().contains("\"apSsid\":\"$ssid\"")
    } finally {
      conn.disconnect()
    }
  }.getOrDefault(false)
}
