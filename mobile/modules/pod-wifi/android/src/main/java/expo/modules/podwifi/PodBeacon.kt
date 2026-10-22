package expo.modules.podwifi

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.net.wifi.WifiManager
import android.os.Build
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.ExistingWorkPolicy
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

// The pod's beacon: its AP's beacons carry a vendor element (webapi.cpp,
// web_beacon_update) - OUI 02:4E:47, type 1, then the last drive's 8-byte
// fingerprint (logcrypt_beacon). The phone reads it from its own WiFi scan
// results, without joining, and says when it names a drive it does not have.
//
// Which fingerprints are known is the app's to say (setKnown): it holds the
// key and the drives. This side only compares, so the check runs with no JS.
object PodBeacon {
  private val OUI = byteArrayOf(0x02, 0x4E, 0x47)
  private const val TYPE: Byte = 1
  private const val PREFS = "podBeacon"
  private const val WORK = "podBeacon"
  private const val WORK_SHORT = "podBeaconShort"
  // How often the background check runs. Under 15 (for testing, 2026-10-22)
  // it is a chain of one-off work, each queuing the next; 15 is the normal.
  private const val CHECK_MINUTES = 15L
  private const val CHANNEL = "newDrives"
  private const val NOTE_ID = 1

  private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

  /**
   * The fingerprint (hex) in the freshest scan result that has one, if it is
   * no older than maxAgeMs; null without one, or without the location
   * permission scan results need.
   */
  fun seen(ctx: Context, maxAgeMs: Long): String? {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
    val wm = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return null
    val results = try {
      wm.scanResults
    } catch (e: SecurityException) {
      return null
    }
    val nowUs = SystemClock.elapsedRealtime() * 1000
    return results
      .filter { nowUs - it.timestamp <= maxAgeMs * 1000 }
      .sortedByDescending { it.timestamp }
      .firstNotNullOfOrNull { r ->
        r.informationElements.firstNotNullOfOrNull { ie ->
          if (ie.id != 221) return@firstNotNullOfOrNull null
          val b = ByteArray(ie.bytes.remaining()).also { ie.bytes.duplicate().get(it) }
          if (b.size < 12 || b[0] != OUI[0] || b[1] != OUI[1] || b[2] != OUI[2] || b[3] != TYPE) null
          else b.copyOfRange(4, 12).joinToString("") { "%02x".format(it) }
        }
      }
  }

  /**
   * The BSSID of the network called `ssid` in the scan results of the last
   * five minutes, for the connect request; null without one, or without the
   * location permission.
   */
  fun bssidOf(ctx: Context, ssid: String): String? {
    val wm = ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return null
    val results = try {
      wm.scanResults
    } catch (e: SecurityException) {
      return null
    }
    val nowUs = SystemClock.elapsedRealtime() * 1000
    @Suppress("DEPRECATION")
    return results
      .filter { it.SSID == ssid && nowUs - it.timestamp <= 5 * 60_000_000L }
      .maxByOrNull { it.timestamp }
      ?.BSSID
  }

  /** Asks for a fresh scan. Android throttles these; a refused one is no harm. */
  fun scan(ctx: Context) {
    @Suppress("DEPRECATION")
    runCatching { (ctx.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager).startScan() }
  }

  /** The fingerprints of the drives the phone has. A note on one goes. */
  fun setKnown(ctx: Context, fps: List<String>) {
    val p = prefs(ctx)
    p.edit().putStringSet("known", fps.toSet()).apply()
    if (p.getString("notified", null) in fps) cancelNote(ctx)
  }

  fun isKnown(ctx: Context, fp: String): Boolean =
    prefs(ctx).getStringSet("known", emptySet())!!.contains(fp)

  /** The background check: on (with the note's words), or off. */
  fun watch(ctx: Context, on: Boolean, title: String, text: String) {
    val wm = WorkManager.getInstance(ctx)
    if (!on) {
      wm.cancelUniqueWork(WORK)
      wm.cancelUniqueWork(WORK_SHORT)
      // Switched on again, the drive the gauge names is told of again.
      prefs(ctx).edit().remove("notified").remove("title").apply()
      cancelNote(ctx)
      return
    }
    prefs(ctx).edit().putString("title", title).putString("text", text).apply()
    if (CHECK_MINUTES >= 15) {
      // 15 minutes is the shortest periodic work WorkManager allows; parked a
      // while, the gauge beacons until it sleeps, so a drive is still caught.
      val req = PeriodicWorkRequestBuilder<BeaconWorker>(CHECK_MINUTES, TimeUnit.MINUTES).build()
      wm.enqueueUniquePeriodicWork(WORK, ExistingPeriodicWorkPolicy.UPDATE, req)
    } else {
      wm.cancelUniqueWork(WORK)
      next(ctx, ExistingWorkPolicy.REPLACE)
    }
  }

  // Shorter than 15 minutes: one-off work that queues the next after itself.
  fun next(ctx: Context, policy: ExistingWorkPolicy) {
    val req = OneTimeWorkRequestBuilder<BeaconWorker>().setInitialDelay(CHECK_MINUTES, TimeUnit.MINUTES).build()
    WorkManager.getInstance(ctx).enqueueUniqueWork(WORK_SHORT, policy, req)
  }

  fun short(ctx: Context) = CHECK_MINUTES < 15 && prefs(ctx).contains("title")

  /** One pass of the background check. */
  fun check(ctx: Context) {
    // Throttled by Android (a few a hour in the background) - it refreshes
    // the results the next pass reads; the system's own scans do too.
    scan(ctx)
    val fp = seen(ctx, 20 * 60_000L) ?: return
    val p = prefs(ctx)
    if (isKnown(ctx, fp) || p.getString("notified", null) == fp) return
    note(ctx, p.getString("title", null) ?: "N Gauge", p.getString("text", null) ?: "")
    p.edit().putString("notified", fp).apply()
  }

  private fun note(ctx: Context, title: String, text: String) {
    val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
      nm.createNotificationChannel(NotificationChannel(CHANNEL, title, NotificationManager.IMPORTANCE_DEFAULT))
    }
    val open = ctx.packageManager.getLaunchIntentForPackage(ctx.packageName)?.apply {
      flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED
    }
    val b = NotificationCompat.Builder(ctx, CHANNEL)
      .setSmallIcon(android.R.drawable.stat_notify_sync)
      .setContentTitle(title)
      .setContentText(text)
      .setAutoCancel(true)
    if (open != null) {
      b.setContentIntent(PendingIntent.getActivity(ctx, 0, open, PendingIntent.FLAG_IMMUTABLE))
    }
    // Without the notification permission this is dropped, quietly.
    runCatching { nm.notify(NOTE_ID, b.build()) }
  }

  private fun cancelNote(ctx: Context) {
    (ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(NOTE_ID)
  }
}

class BeaconWorker(ctx: Context, params: WorkerParameters) : Worker(ctx, params) {
  override fun doWork(): Result {
    PodBeacon.check(applicationContext)
    // After this one finishes, not in its place.
    if (PodBeacon.short(applicationContext)) PodBeacon.next(applicationContext, ExistingWorkPolicy.APPEND_OR_REPLACE)
    return Result.success()
  }
}
