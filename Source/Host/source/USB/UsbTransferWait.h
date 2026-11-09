#pragma once

// An endpoint event alone is not proof that the posted URB has retired. Never
// reuse its DMA buffer when the report is missing, belongs to another URB, or
// has a nonterminal/unknown status. libnx reports terminal statuses 3, 4 and 5.
static bool UsbTransferReportComplete(const UsbDsReportData *report, u32 urbId, bool *fenced)
{
    if (report->report_count <= 8)
    {
        for (u32 i = 0; i < report->report_count; ++i)
        {
            if (report->report[i].id == urbId)
            {
                u32 status = report->report[i].urb_status;
                if (status == 3 || status == 4 || status == 5)
                    return true;
                break;
            }
        }
    }
    *fenced = true;
    return false;
}

// Included after the libnx USB declarations. A failed cancellation/drain leaves
// DMA ownership uncertain: retain the dedicated buffer and fence this endpoint
// until USB is reinitialized. Never clear its event and post another transfer.
static Result UsbTransferWait(UsbDsEndpoint *endpoint, u64 timeout, bool *fenced)
{
    Result rc = eventWait(&endpoint->CompletionEvent, timeout);
    if (R_FAILED(rc))
    {
        Result cancel = usbDsEndpoint_Cancel(endpoint);
        Result drain = eventWait(&endpoint->CompletionEvent, 3000000000ULL);
        if (R_FAILED(cancel) || R_FAILED(drain))
        {
            *fenced = true;
            return R_FAILED(drain) ? drain : cancel;
        }
    }
    eventClear(&endpoint->CompletionEvent);
    return 0;
}
