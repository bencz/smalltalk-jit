# Summarise the JSONL that scripts/ab.sh emits.
#
#   scripts/ab.sh 'a=' 'b=ST_NO_TIER=1' | awk -f scripts/report.awk
#   awk -f scripts/report.awk benchmarks/results/BASELINE.jsonl
#
# Reports, per benchmark and per label, the min / median / p90 of both wall
# clock and retired instructions, then the difference between the two labels.
#
# It REFUSES to call a winner when the two distributions overlap. That is the
# whole point: in this VM a code-layout accident is worth a few percent with
# identical executed instructions, so a median difference smaller than the
# spread inside either label is not evidence of anything. When wall clock is
# inconclusive but instruction counts are not, it says so, because instruction
# counts are deterministic here and are the signal worth acting on.

# Accepts a FRACTIONAL part: ab.sh now records wall clock measured from the
# clock in microseconds and printed as milliseconds with three decimals. An
# integer-only pattern would silently truncate 616.402 to 616 and throw away
# exactly the resolution that measuring the process was meant to buy.
function jnum(line, key,   s) {
	if (match(line, "\"" key "\":-?[0-9]+(\\.[0-9]+)?")) {
		s = substr(line, RSTART, RLENGTH)
		sub(/^.*:/, "", s)
		return s + 0
	}
	return -1
}

function jstr(line, key,   s) {
	if (match(line, "\"" key "\":\"[^\"]*\"")) {
		s = substr(line, RSTART, RLENGTH)
		sub(/^[^:]*:"/, "", s)
		sub(/"$/, "", s)
		return s
	}
	return ""
}

function pct(arr, n, p,   idx) {
	# arr is 1..n and already sorted; nearest-rank, which needs no interpolation
	# and cannot invent a value that was never measured.
	idx = int(p * n + 0.999999)
	if (idx < 1) idx = 1
	if (idx > n) idx = n
	return arr[idx]
}

function summarise(bench, label, what, vals, n,   i, j, tmp, sorted) {
	for (i = 1; i <= n; i++) sorted[i] = vals[i]
	for (i = 2; i <= n; i++) {
		tmp = sorted[i]
		for (j = i - 1; j >= 1 && sorted[j] > tmp; j--) sorted[j + 1] = sorted[j]
		sorted[j + 1] = tmp
	}
	stat_min = sorted[1]
	stat_p10 = pct(sorted, n, 0.1)
	stat_med = pct(sorted, n, 0.5)
	stat_p90 = pct(sorted, n, 0.9)
	stat_max = sorted[n]
}

{
	bench = jstr($0, "bench")
	label = jstr($0, "label")
	if (bench == "" || label == "") next
	ms = jnum($0, "ms")
	insns = jnum($0, "insns")

	key = bench SUBSEP label
	if (!(key in seen)) { seen[key] = 1; order[++nkeys] = key }
	nms[key]++
	MS[key, nms[key]] = ms
	if (insns >= 0) { nin[key]++; IN[key, nin[key]] = insns }

	if (!(bench in benchSeen)) { benchSeen[bench] = 1; benches[++nbench] = bench }
	if (!(label in labelSeen)) { labelSeen[label] = 1; labels[++nlabel] = label }
	gov = jstr($0, "governor"); noturbo = jstr($0, "no_turbo"); cpu = jnum($0, "cpu")
}

END {
	if (nkeys == 0) { print "no records"; exit 1 }
	printf "machine: governor=%s no_turbo=%s pinned-cpu=%d\n\n", gov, noturbo, cpu

	for (b = 1; b <= nbench; b++) {
		bench = benches[b]
		printf "%s\n", bench
		printf "  %-10s %5s  %-28s  %s\n", "label", "runs", "wall ms (min/med/p90)", "instructions (min/med/p90)"

		for (l = 1; l <= nlabel; l++) {
			label = labels[l]
			key = bench SUBSEP label
			if (!(key in seen)) continue
			n = nms[key]
			for (i = 1; i <= n; i++) v[i] = MS[key, i]
			summarise(bench, label, "ms", v, n)
			msMin[label] = stat_min; msMed[label] = stat_med; msP90[label] = stat_p90
			msLine = sprintf("%.1f / %.1f / %.1f", stat_min, stat_med, stat_p90)

			inLine = "n/a"
			inMed[label] = -1
			if (nin[key] > 0) {
				m = nin[key]
				for (i = 1; i <= m; i++) w[i] = IN[key, i]
				summarise(bench, label, "insns", w, m)
				inMin[label] = stat_min; inMed[label] = stat_med; inP90[label] = stat_p90
				inLine = sprintf("%.0f / %.0f / %.0f", stat_min, stat_med, stat_p90)
			}
			printf "  %-10s %5d  %-28s  %s\n", label, n, msLine, inLine
			present[label] = 1
		}

		# verdict, only between the FIRST TWO labels that actually have data
		la = ""; lb = ""
		for (l = 1; l <= nlabel; l++) {
			if (!((bench SUBSEP labels[l]) in seen)) continue
			if (la == "") la = labels[l]; else if (lb == "") lb = labels[l]
		}
		if (lb != "") {
			# PAIRED differences, not a comparison of raw medians.
			#
			# ab.sh runs ABBA so that thermal drift and frequency scaling are
			# COMMON MODE between the two labels inside a round; subtracting run
			# i of one label from run i of the other cancels them. Judging raw
			# medians against the within-label spread throws that cancellation
			# away and calls real effects inconclusive: measured on this machine,
			# a change worth -3.3% instructions gave a +13 ms median delta
			# against a 29 ms within-label spread (verdict: INCONCLUSIVE) while
			# the paired deltas were consistently on one side of zero.
			#
			# The refusal rule stays strict, it just applies to the right
			# quantity: the p10..p90 interval of the PAIRED deltas has to sit
			# entirely on one side of zero. An effect that flips sign between
			# rounds is still called noise.
			ka = bench SUBSEP la; kb = bench SUBSEP lb
			npair = nms[ka] < nms[kb] ? nms[ka] : nms[kb]
			npos = 0; nneg = 0
			for (pi = 1; pi <= npair; pi++) {
				pd[pi] = MS[kb, pi] - MS[ka, pi]
				if (pd[pi] > 0) npos++; else if (pd[pi] < 0) nneg++
			}
			summarise(bench, lb, "paired", pd, npair)
			dMed = stat_med

			# SIGN TEST on the paired deltas, not a percentile interval.
			#
			# A percentile interval degenerates at the sample sizes this harness
			# actually produces: with nearest-rank and n=8, p10 IS the minimum
			# and p90 IS the maximum, so "the interval must not cross zero"
			# silently became "not one single pair may disagree" and refused
			# every real result. The sign test asks the right question at small
			# n: how surprising is it that the difference landed on the same
			# side this many times, if the two configurations were identical?
			# Threshold is the normal approximation to the two-sided test at 5%,
			# which reproduces the exact binomial critical values over the range
			# that matters here (n=8 needs 7, n=12 needs 10, n=20 needs 15).
			nmaj = npos > nneg ? npos : nneg
			need = int(npair / 2.0 + 0.98 * sqrt(npair) + 0.999999)
			if (npair < 6 || nmaj < need) {
				printf "  wall clock: INCONCLUSIVE (paired delta median %+.1f ms, %d of %d runs agree in sign, %d needed, n=%d)\n",
					dMed, nmaj, npair, need, npair
			} else {
				# NOTE the parentheses: inside printf's argument list an
				# unparenthesised `>` is parsed as output redirection.
				printf "  wall clock: %s is %+.1f%% vs %s (paired delta median %+.1f ms, %d of %d runs agree, n=%d)\n",
					lb, (msMed[la] > 0 ? 100.0 * dMed / msMed[la] : 0), la, dMed, nmaj, npair, npair
			}
			if (inMed[la] > 0 && inMed[lb] > 0) {
				idelta = inMed[lb] - inMed[la]
				printf "  instructions: %s is %+.3f%% vs %s (median %.0f -> %.0f)\n",
					lb, 100.0 * idelta / inMed[la], la, inMed[la], inMed[lb]
			}
		}
		printf "\n"
		delete present
	}
}
