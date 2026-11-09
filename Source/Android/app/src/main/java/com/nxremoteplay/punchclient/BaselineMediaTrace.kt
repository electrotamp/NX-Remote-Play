package com.nxremoteplay.punchclient

import java.io.Writer

// Preserve the exported summary-file format without detailed research traces.
internal object BaselineMediaTrace {

    const val OUTPUT_HEADER = "output_number,received_video_count,output_pts_us,release_ns\n"
    const val PACKET_HEADER =
        "wire_packet,type,video_candidate,bytes,header_begin_ns,header_end_ns,payload_end_ns,handled_ns,cpu_begin_ns,cpu_end_ns,available_before_header,host_timestamp_raw\n"

    fun writeDisabled(writer: Writer, outputs: Boolean) {
        writer.write(
            "# media-trace detailed_media_trace=disabled records_not_collected; original_summary_diagnostics_retained\n")
        writer.write(if (outputs) OUTPUT_HEADER else PACKET_HEADER)
    }
}
