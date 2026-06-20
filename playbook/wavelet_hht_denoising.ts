/**
 * @strategy wavelet_hht_denoising.ts
 *
 * Implements Wavelet Denoising and HHT (Dragon) Trend Extraction.
 * Based on Category C research from QuantsPlaybook.
 */

import("timeseries");

function update() {
    // 1. Wavelet Denoising (Haar Transform)
    // Extract approximation (trend) and remove detail (noise)
    var dwt_res = finance.ts_dwt(CLOSE, 3); // 3 levels of decomposition
    var approx = dwt_res[0]; // The smoothed signal
    
    // 2. HHT / EMD (Dragon Logic)
    // Decompose into Intrinsic Mode Functions (IMFs)
    var emd_res = finance.ts_emd(CLOSE, 2); 
    // Flat layout: emd_res[0..n-1] = IMF1, emd_res[n..2n-1] = Residue (trend)
    var n = len(CLOSE);
    var imf1 = slice(emd_res, 0, n);
    var trend = slice(emd_res, n, n * 2);

    /* ── Trading Strategy ─────────────────────────────────────────────── */

    // Use Wavelet Approximation for "Responsive" smoothing 
    // vs EMD Trend for "Structural" direction.
    
    if (is_flat()) {
        // Condition: Wavelet signal turning up while structural trend is bullish
        if (last(approx) > prev(approx) && last(trend) > prev(trend) && last(CLOSE) > last(trend)) {
            buy(1.0, 0, 0);
        }
    } 
    
    else if (is_long()) {
        // Exit if Wavelet denoised signal breaks down (Early warning)
        if (last(approx) < prev(approx)) {
            flat();
        }
    }
}

update();
