// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

package org.ppsspp.ppsspp;

import android.Manifest;
import android.content.Intent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Process;
import android.provider.Settings;
import android.util.Log;
import android.view.Choreographer;
import android.view.SurfaceView;
import androidx.appcompat.app.AlertDialog;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Optional display output; never loads vendor code into PPSSPP. */
final class DisplayXRSupport {
	static final int CAMERA_REQUEST = 731;
	private static final String TAG = "DisplayXR";
	private static final String RUNTIME = "org.freedesktop.monado.openxr_runtime.out_of_process";
	private static final String GUIDE = "https://github.com/DisplayXR/displayxr-installer/blob/main/android-installer/README.md";
	private final PpssppActivity activity;
	private final SurfaceView surface;
	private final ExecutorService worker = Executors.newSingleThreadExecutor();
	private static volatile java.lang.ref.WeakReference<DisplayXRSupport> current = new java.lang.ref.WeakReference<>(null);
	private volatile boolean closed;
	private boolean receiverRegistered;
	private boolean resumed;
	private boolean probing;
	private int probeEpoch;
	private AlertDialog trackingPrompt;
	private boolean cameraRequested;
	private boolean cameraInFlight;
	private boolean cameraPending;
	private AlertDialog suggestion;
	private final int[] location = new int[2];
	private final BroadcastReceiver packages = new BroadcastReceiver() {
		@Override public void onReceive(Context context, Intent intent) {
			Uri data = intent.getData();
			if (resumed && data != null && RUNTIME.equals(data.getSchemeSpecificPart())) {
				resume(false);
			}
		}
	};
	private final Choreographer.FrameCallback geometry = new Choreographer.FrameCallback() {
		@Override public void doFrame(long frameTimeNanos) {
			if (closed || !resumed) {
				return;
			}
			surface.getLocationOnScreen(location);
			nativeGeometry(location[0], location[1], surface.getWidth(), surface.getHeight(),
				surface.getDisplay() != null ? surface.getDisplay().getDisplayId() : 0);
			Choreographer.getInstance().postFrameCallback(this);
		}
	};

	private static native void nativeEnvironment(DisplayXRSupport owner, PpssppActivity activity,
		boolean eligible, boolean installed, boolean resumed, boolean camera);
	private static native void nativeEligible(boolean eligible);
	private static native void nativeProbe(PpssppActivity activity);
	private static native void nativeGeometry(int x, int y, int width, int height, int display);

	DisplayXRSupport(PpssppActivity activity, SurfaceView surface, Bundle savedState) {
		current = new java.lang.ref.WeakReference<>(this);
		this.activity = activity;
		this.surface = surface;
		if (savedState != null) {
			cameraRequested = savedState.getBoolean("displayxr.cameraRequested");
			cameraInFlight = savedState.getBoolean("displayxr.cameraInFlight");
			cameraPending = savedState.getBoolean("displayxr.cameraPending");
		}
		publishEnvironment();
	}

	private boolean installed(String name) {
		try {
			return activity.getPackageManager().getApplicationInfo(name, 0).enabled;
		} catch (PackageManager.NameNotFoundException e) {
			return false;
		}
	}

	// Before NativeApp.init, which creates the Vulkan instance and device: what DisplayXR needs is only enabled then.
	static void detect(Context context) {
		nativeEligible(eligible(context));
	}

	private boolean eligible() {
		return eligible(activity);
	}

	private static boolean platformSupported() {
		return Build.VERSION.SDK_INT >= 26 && Process.is64Bit() &&
			System.getProperty("os.arch", "").contains("aarch64");
	}

	private static boolean knownDisplay(Context context) {
		try {
			ApplicationInfo info = context.getPackageManager().getApplicationInfo("com.leialoft.display.config", 0);
			return info.enabled && (info.flags & ApplicationInfo.FLAG_SYSTEM) != 0;
		} catch (PackageManager.NameNotFoundException e) {
			return false;
		}
	}

	private static boolean eligible(Context context) {
		if (!platformSupported()) { return false; }
		try {
			if (context.getPackageManager().getApplicationInfo(RUNTIME, 0).enabled) { return true; }
		} catch (PackageManager.NameNotFoundException e) {
			// A known panel can offer installation before the runtime is present.
		}
		return knownDisplay(context);
	}

	private void publishEnvironment() {
		nativeEnvironment(this, activity, eligible(), installed(RUNTIME), resumed,
			ContextCompat.checkSelfPermission(activity, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED);
	}

	void resume() { resume(true); }

	private void resume(boolean allowSuggestion) {
		if (closed || current.get() != this) {
			return;
		}
		resumed = true;
		probeEpoch++;
		boolean eligible = eligible();
		boolean present = eligible && installed(RUNTIME);
		publishEnvironment();
		if (!platformSupported()) {
			return;
		}
		if (!receiverRegistered) {
			IntentFilter filter = new IntentFilter();
			filter.addAction(Intent.ACTION_PACKAGE_ADDED);
			filter.addAction(Intent.ACTION_PACKAGE_REMOVED);
			filter.addAction(Intent.ACTION_PACKAGE_REPLACED);
			filter.addAction(Intent.ACTION_PACKAGE_CHANGED);
			filter.addDataScheme("package");
			ContextCompat.registerReceiver(activity, packages, filter, ContextCompat.RECEIVER_NOT_EXPORTED);
			receiverRegistered = true;
		}
		Choreographer.getInstance().removeFrameCallback(geometry);
		if (present) { Choreographer.getInstance().postFrameCallback(geometry); }
		SharedPreferences prefs = activity.getSharedPreferences("displayxr", 0);
		if (allowSuggestion && knownDisplay(activity) && !present && !prefs.getBoolean("suggested", false)) {
			prefs.edit().putBoolean("suggested", true).apply();
			suggestion = new AlertDialog.Builder(activity)
				.setTitle("Glasses-free 3D")
				.setMessage("Install DisplayXR to use glasses-free 3D on this device.")
				.setPositiveButton("Installation guide", (dialog, which) -> openGuide())
				.setNegativeButton("Not now", null).show();
		}
		if (present) { startProbe(); }
		if (cameraPending) { launchCameraRequest(); }
	}

	private void startProbe() {
		if (closed || !resumed || probing || current.get() != this || !installed(RUNTIME)) { return; }
		probing = true;
		final int epoch = probeEpoch;
		worker.execute(() -> {
			if (closed || current.get() != this) { return; }
			nativeProbe(activity);
			activity.runOnUiThread(() -> {
				probing = false;
				if (!closed && resumed && epoch != probeEpoch) { startProbe(); }
			});
		});
	}

	void saveState(Bundle state) {
		state.putBoolean("displayxr.cameraRequested", cameraRequested && trackingPrompt == null);
		state.putBoolean("displayxr.cameraInFlight", cameraInFlight);
		state.putBoolean("displayxr.cameraPending", cameraPending);
	}

	void pause() {
		if (current.get() != this || closed) { return; }
		resumed = false;
		probeEpoch++;
		Choreographer.getInstance().removeFrameCallback(geometry);
		publishEnvironment();
	}

	void close() {
		pause();
		closed = true;
		if (receiverRegistered) {
			activity.unregisterReceiver(packages);
			receiverRegistered = false;
		}
		if (suggestion != null) {
			suggestion.dismiss();
		}
		if (trackingPrompt != null) { trackingPrompt.dismiss(); }
		worker.shutdownNow();
	}

	private void openGuide() {
		try {
			activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(GUIDE)));
		} catch (android.content.ActivityNotFoundException e) {
			android.widget.Toast.makeText(activity, GUIDE, android.widget.Toast.LENGTH_LONG).show();
		}
	}

	// Called from native threads; all Activity and Surface work stays on the UI thread.
	void retry() {
		activity.runOnUiThread(() -> {
			if (!closed && resumed && current.get() == this) {
				if (!cameraInFlight && trackingPrompt == null) { cameraRequested = false; }
				resume(false);
			}
		});
	}

	void requestCamera() {
		activity.runOnUiThread(() -> {
			if (closed || current.get() != this || cameraRequested || cameraInFlight) { return; }
			if (!resumed) {
				Log.i(TAG, "Camera request deferred until resume");
				return;
			}
			cameraRequested = true;
			if (ContextCompat.checkSelfPermission(activity, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
				publishEnvironment();
				return;
			}
			SharedPreferences prefs = activity.getSharedPreferences("displayxr", 0);
			boolean rationale = ActivityCompat.shouldShowRequestPermissionRationale(activity, Manifest.permission.CAMERA);
			if (prefs.getBoolean("cameraRequested", false) && !rationale) {
				Log.i(TAG, "Camera permission cannot be requested again; offering app settings");
				showCameraSettings();
			} else if (rationale) {
				trackingPrompt = new AlertDialog.Builder(activity).setTitle("DisplayXR eye tracking")
					.setMessage("DisplayXR needs camera permission to track your eyes for glasses-free 3D.")
					.setPositiveButton("Continue", (dialog, which) -> {
						cameraPending = true;
						surface.post(this::launchCameraRequest);
					})
					.setNegativeButton("Not now", null).create();
				trackingPrompt.setOnDismissListener(dialog -> trackingPrompt = null);
				trackingPrompt.show();
			} else {
				cameraPending = true;
				launchCameraRequest();
			}
		});
	}

	private void launchCameraRequest() {
		if (closed || current.get() != this || !resumed || cameraInFlight || !cameraPending) { return; }
		cameraPending = false;
		cameraInFlight = true;
		activity.getSharedPreferences("displayxr", 0).edit().putBoolean("cameraRequested", true).apply();
		Log.i(TAG, "Requesting Android camera permission");
		ActivityCompat.requestPermissions(activity, new String[] { Manifest.permission.CAMERA }, CAMERA_REQUEST);
	}

	void cameraPermissionResult(int[] grantResults) {
		if (closed || current.get() != this) { return; }
		cameraInFlight = false;
		cameraRequested = true;
		if (grantResults.length == 0) {
			activity.getSharedPreferences("displayxr", 0).edit().remove("cameraRequested").apply();
		}
		boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
		Log.i(TAG, "Camera permission result: " + (granted ? "granted" : grantResults.length == 0 ? "cancelled" : "denied"));
		publishEnvironment();
		if (!granted && grantResults.length > 0 && resumed &&
			!ActivityCompat.shouldShowRequestPermissionRationale(activity, Manifest.permission.CAMERA)) {
			showCameraSettings();
		}
	}

	private void showCameraSettings() {
		if (trackingPrompt != null || closed || !resumed) { return; }
		trackingPrompt = new AlertDialog.Builder(activity).setTitle("DisplayXR eye tracking")
			.setMessage("Camera permission is disabled. Enable it in Android app settings to use DisplayXR eye tracking.")
			.setPositiveButton("App settings", (dialog, which) -> {
				Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + activity.getPackageName()));
				activity.startActivity(intent);
			})
			.setNegativeButton("Not now", null).create();
		trackingPrompt.setOnDismissListener(dialog -> trackingPrompt = null);
		trackingPrompt.show();
	}

	void resizeSurface() {
		activity.runOnUiThread(() -> {
			if (!closed && resumed && current.get() == this) {
				activity.refreshDisplayXRSurface();
			}
		});
	}
}
