using System.Collections.Concurrent;
using System.Net;

namespace Rendezvous;

// Fixed-window rate limiting per source IP limits password guesses and registration
// floods. The session cap separately bounds memory across many source addresses.
class RateLimiter
{
	class Bucket
	{
		public int Count;
		public DateTime WindowStartUtc;
	}

	readonly ConcurrentDictionary<IPAddress, Bucket> _buckets = new();
	readonly int _maxPerWindow;
	readonly TimeSpan _window;

	public RateLimiter(int maxPerWindow, TimeSpan window)
	{
		_maxPerWindow = maxPerWindow;
		_window = window;
	}

	// True if this address is still under its limit for the current window (and counts this
	// call toward it), false if it should be refused.
	public bool Allow(IPAddress address)
	{
		var now = DateTime.UtcNow;
		var bucket = _buckets.GetOrAdd(address, _ => new Bucket { WindowStartUtc = now });

		lock (bucket)
		{
			if (now - bucket.WindowStartUtc > _window)
			{
				bucket.WindowStartUtc = now;
				bucket.Count = 0;
			}

			bucket.Count++;
			return bucket.Count <= _maxPerWindow;
		}
	}

	// Called periodically so long-idle source addresses don't sit in memory forever.
	public void SweepIdle(TimeSpan idleAfter)
	{
		var cutoff = DateTime.UtcNow - idleAfter;
		foreach (var (address, bucket) in _buckets)
		{
			lock (bucket)
			{
				if (bucket.WindowStartUtc < cutoff)
					_buckets.TryRemove(address, out _);
			}
		}
	}
}
