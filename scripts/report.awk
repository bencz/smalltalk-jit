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

function jnum(line, key,   s) {
	if (match(line, "\"" key "\":-?[0-9]+")) {
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
			msLine = sprintf("%d / %d / %d", stat_min, stat_med, stat_p90)

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
			# Wall clock: inconclusive when the medians differ by less than the
			# within-label spread (p90 - min), which is the honest floor for
			# "this machine can tell these apart".
			spread = (msP90[la] - msMin[la])
			if ((msP90[lb] - msMin[lb]) > spread) spread = (msP90[lb] - msMin[lb])
			delta = msMed[lb] - msMed[la]
			absd = delta < 0 ? -delta : delta
			if (absd <= spread) {
				printf "  wall clock: INCONCLUSIVE (median delta %+d ms, within-label spread %d ms)\n",
					delta, spread
			} else {
				# NOTE the parentheses: inside printf's argument list an
				# unparenthesised `>` is parsed as output redirection.
				printf "  wall clock: %s is %+.1f%% vs %s (median %d -> %d)\n",
					lb, (msMed[la] > 0 ? 100.0 * delta / msMed[la] : 0), la, msMed[la], msMed[lb]
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
