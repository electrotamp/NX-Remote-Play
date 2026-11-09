#include "udpm_sender.h"
#include "udpm_fec.h"
#include <string.h>

void UdpmSenderDefaultConfig(UdpmSenderConfig* c)
{
	c->shard_size = UDPM_DEFAULT_SHARD_SIZE;
	c->max_data_shards = 32;
	c->fec_percent = 20;
	c->min_parity = 2; // small P-frame blocks still survive a two-packet burst
	c->max_parity = 12;
	c->pace_bps = 20000000;   // 2x the 10 Mbps video target: a frame leaves in ~half a frame time
	c->burst_bytes = 16 * 1024;
}

void UdpmPacerInit(UdpmPacer* p, uint32_t rate_bps, uint32_t burst_bytes)
{
	memset(p, 0, sizeof(*p));
	p->rate_bps = rate_bps ? rate_bps : 1;
	p->burst_bytes = burst_bytes;
}

uint64_t UdpmPacerDelayUs(UdpmPacer* p, uint64_t now_us, uint32_t bytes)
{
	const uint64_t cap = (uint64_t)p->burst_bytes * 1000000ULL;
	if (!p->primed) { p->tokens_x1e6 = cap; p->last_us = now_us; p->primed = 1; }
	if (now_us > p->last_us) {
		// bytes/s * us = bytes * 1e6
		const uint64_t add = (now_us - p->last_us) * (p->rate_bps / 8);
		p->tokens_x1e6 = p->tokens_x1e6 + add > cap ? cap : p->tokens_x1e6 + add;
		p->last_us = now_us;
	}
	const uint64_t need = (uint64_t)bytes * 1000000ULL;
	if (p->tokens_x1e6 >= need) { p->tokens_x1e6 -= need; return 0; }
	const uint64_t bytesPerSec = p->rate_bps / 8 ? p->rate_bps / 8 : 1;
	return (need - p->tokens_x1e6 + bytesPerSec - 1) / bytesPerSec;
}

void UdpmSenderInit(UdpmSender* s, const UdpmSenderConfig* cfg, uint32_t session,
	UdpmRtxSlot* rtx, uint32_t rtx_count,
	UdpmSendFn send, UdpmNowUsFn now, UdpmSleepUsFn sleep, void* io)
{
	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	if (s->cfg.shard_size == 0 || s->cfg.shard_size > UDPM_MAX_SHARD_SIZE) s->cfg.shard_size = UDPM_DEFAULT_SHARD_SIZE;
	if (s->cfg.max_data_shards == 0 || s->cfg.max_data_shards > UDPM_MAX_DATA_SHARDS) s->cfg.max_data_shards = 32;
	if (s->cfg.max_parity > UDPM_MAX_PARITY_SHARDS) s->cfg.max_parity = UDPM_MAX_PARITY_SHARDS;
	if (s->cfg.min_parity > s->cfg.max_parity) s->cfg.min_parity = s->cfg.max_parity;
	s->session = session;
	s->rtx = rtx; s->rtx_count = rtx ? rtx_count : 0;
	if (rtx) memset(rtx, 0, sizeof(*rtx) * rtx_count);
	s->send = send; s->now = now; s->sleep = sleep; s->io = io;
	UdpmPacerInit(&s->pacer, s->cfg.pace_bps, s->cfg.burst_bytes);
	UdpmGfInit();
}

unsigned UdpmSenderParityFor(const UdpmSenderConfig* c, unsigned k)
{
	unsigned m = (k * c->fec_percent + 99) / 100;
	if (m < c->min_parity) m = c->min_parity;
	if (m > c->max_parity) m = c->max_parity;
	if (k + m > 255) m = 255 - k;
	return m;
}

void UdpmSenderSetPace(UdpmSender* s, uint32_t pace_bps)
{
	s->cfg.pace_bps = pace_bps;
	s->pacer.rate_bps = pace_bps ? pace_bps : 1;
}

static void cache(UdpmSender* s, const UdpmShardHeader* h, const uint8_t* dg, size_t len)
{
	if (!s->rtx_count) return;
	UdpmRtxSlot* slot = &s->rtx[s->rtx_next];
	s->rtx_next = (s->rtx_next + 1) % s->rtx_count;
	slot->type = h->type; slot->frame_id = h->frame_id;
	slot->block_index = h->block_index; slot->shard_index = h->shard_index;
	slot->len = (uint16_t)len; slot->valid = 1;
	memcpy(slot->data, dg, len);
}

static int emit(UdpmSender* s, uint8_t* dg, size_t len)
{
	const uint64_t delay = UdpmPacerDelayUs(&s->pacer, s->now(s->io), (uint32_t)len);
	if (delay) {
		s->sleep(s->io, delay);
		s->stats.paced_wait_us += delay;
		UdpmPacerDelayUs(&s->pacer, s->now(s->io), (uint32_t)len); // consume after waiting
	}
	const int rc = s->send(s->io, dg, len);
	if (rc < 0) { return rc; }
	s->stats.bytes += len;
	return 0;
}

int64_t UdpmSenderSendFrame(UdpmSender* s, uint8_t type, const uint8_t* frame, uint32_t size,
	uint64_t timestamp_us, uint8_t flags)
{
	if (type < UdpmType_Video || type > UdpmType_Probe || !size) return -1;
	const unsigned S = s->cfg.shard_size, K = s->cfg.max_data_shards;
	const uint32_t blockBytes = S * K;
	const uint32_t blocks = (size + blockBytes - 1) / blockBytes;
	if (blocks > 255) return -1;
	const uint32_t frameId = s->next_frame_id[type]++;

	UdpmShardHeader h;
	memset(&h, 0, sizeof(h));
	h.type = type; h.session = s->session; h.frame_id = frameId; h.frame_size = size;
	h.block_count = (uint8_t)blocks; h.shard_size = (uint16_t)S; h.timestamp_us = timestamp_us;

	for (uint32_t b = 0; b < blocks; b++) {
		const uint32_t off = b * blockBytes;
		const uint32_t len = size - off < blockBytes ? size - off : blockBytes;
		const unsigned k = (len + S - 1) / S;
		const unsigned m = UdpmSenderParityFor(&s->cfg, k);
		const uint8_t* data[UDPM_MAX_DATA_SHARDS];
		for (unsigned d = 0; d < k; d++) {
			const uint32_t so = off + d * S;
			if (size - so >= S) data[d] = frame + so;
			else { // last shard of the frame: zero-pad a copy
				memset(s->pad, 0, S);
				memcpy(s->pad, frame + so, size - so);
				data[d] = s->pad;
			}
		}
		uint8_t* par[UDPM_MAX_PARITY_SHARDS];
		for (unsigned p = 0; p < m; p++) par[p] = s->parity[p];
		if (m) UdpmFecEncode(data, k, par, m, S);

		h.block_index = (uint8_t)b; h.data_shards = (uint8_t)k; h.parity_shards = (uint8_t)m;
		for (unsigned i = 0; i < k + m; i++) {
			h.shard_index = (uint8_t)i;
			h.flags = (uint8_t)((flags & UdpmFlag_Keyframe) | (i >= k ? UdpmFlag_Parity : 0));
			h.packet_seq = s->packet_seq[type]++;
			UdpmWriteShardHeader(s->datagram, &h);
			memcpy(s->datagram + UDPM_HEADER_SIZE, i < k ? data[i] : par[i - k], S);
			const size_t dlen = UDPM_HEADER_SIZE + S;
			cache(s, &h, s->datagram, dlen);
			if (emit(s, s->datagram, dlen) < 0) return -1;
		}
	}
	s->stats.frames++;
	return frameId;
}

int UdpmSenderHandleNack(UdpmSender* s, const UdpmNack* n)
{
	int resent = 0;
	for (uint32_t i = 0; i < s->rtx_count; i++) {
		UdpmRtxSlot* slot = &s->rtx[i];
		if (!slot->valid || slot->type != n->stream_type || slot->frame_id != n->frame_id || slot->block_index != n->block_index)
			continue;
		const unsigned idx = slot->shard_index;
		const uint64_t bit = idx < 64 ? (n->missing_lo >> idx) & 1 : (n->missing_hi >> (idx - 64)) & 1;
		if (!bit) continue;
		memcpy(s->datagram, slot->data, slot->len);
		s->datagram[3] |= UdpmFlag_Retransmit;
		const uint32_t seq = s->packet_seq[slot->type]++;
		for (int j = 0; j < 4; j++) s->datagram[8 + j] = (uint8_t)(seq >> (8 * j));
		if (emit(s, s->datagram, slot->len) < 0) break;
		s->stats.retransmits++;
		resent++;
	}
	return resent;
}
