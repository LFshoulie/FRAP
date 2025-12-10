#!/usr/bin/env python3
# tools/frap_table_generator.py
# 修正版：基于论文 Eq.(9)/(10) 的近似实现（包含 back-to-back hit）
# 并使用 Cbar (overline{C}) 计算 slack（与论文一致）
# 输出：C 头文件 frap_table_generated.h（静态表）
#
# Usage:
#   python3 tools/frap_table_generator.py <config.json> <out_header.h>

import sys
import math
import json
from collections import defaultdict

# ---------------- argument parsing ----------------
if len(sys.argv) != 3:
    print("Usage: frap_table_generator.py <config.json> <out_header.h>")
    sys.exit(2)

cfg_path = sys.argv[1]
out_h = sys.argv[2]

with open(cfg_path, 'r') as f:
    cfg = json.load(f)

# Expected cfg keys: "cpus", "tasks", "resources"
tasks = cfg.get("tasks", [])
resources = { r["id"]: r for r in cfg.get("resources", []) }
cpus = cfg.get("cpus", [])

# build tasks_by_cpu and name2task
tasks_by_cpu = defaultdict(list)
name2task = {}
for t in tasks:
    tasks_by_cpu[t["cpu"]].append(t)
    name2task[t["name"]] = t

# helpers: local higher/lower priority (based on base P)
def local_hp(task):
    cpu_tasks = tasks_by_cpu[task["cpu"]]
    return [x for x in cpu_tasks if x["P"] > task["P"]]

def local_lp(task):
    cpu_tasks = tasks_by_cpu[task["cpu"]]
    return [x for x in cpu_tasks if x["P"] < task["P"]]

# ---------------- core numerical helpers (corrected) ----------------

def phi_of(task, resid):
    """φ_k(τ) = N_{τ,k} / T_τ"""
    Nk = int(task.get("req", {}).get(str(resid), 0))
    if Nk == 0:
        return 0.0
    return float(Nk) / float(task["T"])

def phi_cpu_for_task(task_i, cpu, resid, include_back_to_back=True):
    """
    Compute phi^k(Γ_m) for cpu `cpu` relative to analyzed task_i.
    If include_back_to_back True, add back-to-back term Nk_j / T_i for each remote task j.
    This follows the paper's suggestion to consider an extra back-to-back hit during tau_i's release.
    """
    s = 0.0
    Ti = float(task_i["T"])
    for t in tasks_by_cpu[cpu]:
        Nk = int(t.get("req", {}).get(str(resid), 0))
        if Nk == 0:
            continue
        # normal contribution
        s += float(Nk) / float(t["T"])
        # back-to-back: at most one additional occurrence within a release of task_i
        if include_back_to_back:
            s += float(Nk) / Ti
    return s

def spin_prio_of(task, resid, Pcfg):
    """Get spin priority of (task,resid) from Pcfg, fallback to base priority."""
    key = (task["name"], resid)
    return Pcfg.get(key, task["P"])

def build_Gamma_and_threshold(task_i, resid, Pcfg):
    """
    Build Gamma^k_i: candidate tasks that could preempt spinning set (tau_i ∪ lhp(i)),
    and compute P_threshold = max spin-prio among tau_i ∪ lhp(i).
    Gamma includes all tasks (on any cpu) that access resid and whose spin_prio > P_threshold.
    """
    local_set = [task_i] + local_hp(task_i)
    # compute threshold based on current Pcfg (current spin priorities)
    P_threshold = max(spin_prio_of(x, resid, Pcfg) for x in local_set)
    Gamma = []
    for t in tasks:
        if int(t.get("req", {}).get(str(resid), 0)) > 0:
            if spin_prio_of(t, resid, Pcfg) > P_threshold:
                Gamma.append(t)
    return Gamma, P_threshold

def compute_tilde_w(task_i, resid, Pcfg):
    """
    Compute \tilde w^k_i as Eq.(9) approximated and grouped per remote CPU:
      tilde_w = sum_{m != A_i} min{ sum_{h in Gamma ∩ cpu_m} 1/Th,
                                     [ phi^k(Gamma_m) - phi^k(tau_i ∪ lhp(i)) ]_0 }
    """
    Gamma, _ = build_Gamma_and_threshold(task_i, resid, Pcfg)
    # phi_local = phi^k(tau_i ∪ lhp(i))
    phi_local = sum(phi_of(x, resid) for x in ([task_i] + local_hp(task_i)))

    total = 0.0
    for cpu in tasks_by_cpu.keys():
        if cpu == task_i["cpu"]:
            continue
        # sum of 1/Th for Gamma members located on this cpu
        sum_invTh_cpu = 0.0
        for h in Gamma:
            if h["cpu"] == cpu:
                sum_invTh_cpu += 1.0 / float(h["T"])
        # phi^k(Gamma_m) with back-to-back considered relative to task_i
        phi_gamma_m = phi_cpu_for_task(task_i, cpu, resid, include_back_to_back=True)
        rem = phi_gamma_m - phi_local
        if rem < 0.0:
            rem = 0.0
        total += min(sum_invTh_cpu, rem)
    return total

def compute_tilde_b(task_i, resid, Pcfg):
    """
    Compute \tilde b^k_i as Eq.(10):
      btilde = 1/Ti + sum_{m != Ai} min{ 1/Ti, [ phi^k(Gamma_m) - phi^k(tau_i ∪ lhp(i)) - sum_{h in Gamma ∩ cpu_m} 1/Th ]_0 }
    """
    Ti = float(task_i["T"])
    base = 1.0 / Ti
    Gamma, _ = build_Gamma_and_threshold(task_i, resid, Pcfg)
    phi_local = sum(phi_of(x, resid) for x in ([task_i] + local_hp(task_i)))

    add = 0.0
    for cpu in tasks_by_cpu.keys():
        if cpu == task_i["cpu"]:
            continue
        sum_invTh_cpu = sum((1.0 / float(h["T"])) for h in Gamma if h["cpu"] == cpu)
        phi_gamma_m = phi_cpu_for_task(task_i, cpu, resid, include_back_to_back=True)
        val = phi_gamma_m - phi_local - sum_invTh_cpu
        if val < 0.0:
            val = 0.0
        add += min(1.0 / Ti, val)
    return base + add

def compute_Psi(task_i, Pcfg):
    """
    Compute Psi(taui) per the paper approx:
      Psi = sum_k e_tilde_ki * c_k + sum_k w_tilde_ki * c_k + max_k b_tilde_ki * c_k
    Return (Psi_val, b_dict) where b_dict[resid] = b_tilde for that resid.
    """
    e_sum = 0.0
    w_sum = 0.0
    b_dict = {}

    # iterate only over resources task_i requests (practical)
    for resid_str, Nk in task_i.get("req", {}).items():
        resid = int(resid_str)
        ck = float(resources[resid]["c"])

        # phi_local = phi^k(tau_i ∪ lhp(i))
        phi_local = sum(phi_of(x, resid) for x in ([task_i] + local_hp(task_i)))

        # e_tilde = sum_{m != Ai} min{ phi_local, phi^k(Gamma_m) }
        e_tilde = 0.0
        for cpu in tasks_by_cpu.keys():
            if cpu == task_i["cpu"]:
                continue
            phi_gamma_m = phi_cpu_for_task(task_i, cpu, resid, include_back_to_back=True)
            e_tilde += min(phi_local, phi_gamma_m)
        e_sum += e_tilde * ck

        # compute w_tilde and b_tilde using the helper functions
        w_tilde = compute_tilde_w(task_i, resid, Pcfg)
        b_tilde = compute_tilde_b(task_i, resid, Pcfg)

        w_sum += w_tilde * ck
        b_dict[int(resid)] = b_tilde

    # b part is max over resource group (paper uses max over Fb). Here we take max b_tilde * c_k
    if b_dict:
        b_max_ck = max((b_dict[r] * float(resources[r]["c"])) for r in b_dict.keys())
    else:
        b_max_ck = 0.0

    Psi_val = e_sum + w_sum + b_max_ck
    return Psi_val, b_dict

# ---------------- compute Cbar (overline{C}) and slack helpers ----------------

def compute_Cbar(task):
    """
    Compute \overline{C}_task = C_task + sum_{res in req} N_task,res * c_res
    Returns float (same time unit as C and c_k in config).
    """
    Ci = float(task.get("C", 0.0))
    extra = 0.0
    for resid_str, Nk in task.get("req", {}).items():
        resid = int(resid_str)
        ck = float(resources[resid]["c"])
        extra += float(Nk) * ck
    return Ci + extra

# Precompute Cbar for all tasks
Cbar_map = {}
for t in tasks:
    Cbar_map[t["name"]] = compute_Cbar(t)

def slack(task):
    """
    Slack S_i = max(0, D_i - Cbar_i - sum_{h in lhp(i)} ceil(Ti/Th) * Cbar_h)
    Uses precomputed Cbar_map for higher-priority tasks.
    """
    Di = float(task.get("D", 0.0))
    Ti = float(task.get("T", 0.0))
    Cbar_i = Cbar_map.get(task["name"], float(task.get("C", 0.0)))

    sum_ch = 0.0
    for h in local_hp(task):
        Th = float(h["T"])
        Cbar_h = Cbar_map.get(h["name"], float(h.get("C", 0.0)))
        sum_ch += math.ceil(Ti / Th) * Cbar_h

    s = Di - Cbar_i - sum_ch
    return max(0.0, s)

# ---------------- initialize P_table (initial guess) ----------------
global_max_prio = max((t["P"] for t in tasks), default=255)
P_HIGH = global_max_prio

P_table = {}
for t in tasks:
    for resid_str, Nk in t.get("req", {}).items():
        resid = int(resid_str)
        # phi_local_set = phi(ti ∪ lhp(i))
        phi_local_set = phi_of(t, resid) + sum(phi_of(h, resid) for h in local_hp(t))
        ok_all = True
        # check if phi_local_set >= phi_cpu(m) for every remote cpu
        for cpu in tasks_by_cpu.keys():
            if cpu == t["cpu"]:
                continue
            # here include_back_to_back True for conservative init
            if phi_local_set + 1e-12 < phi_cpu_for_task(t, cpu, resid, include_back_to_back=True):
                ok_all = False
                break
        if ok_all:
            P_table[(t["name"], resid)] = t["P"]
        else:
            P_table[(t["name"], resid)] = P_HIGH

# ---------------- linear search / iterative refinement ----------------
Pcfg = dict(P_table)  # working copy

for cpu in list(tasks_by_cpu.keys()):
    # order tasks on this cpu by descending base priority
    ordered = sorted(tasks_by_cpu[cpu], key=lambda x: x["P"], reverse=True)
    for task_i in ordered:
        # Build initial Fstar = { rk | rk ∈ F(llp(i)) and Pk_l >= Pi }
        Fstar = set()
        for lp in local_lp(task_i):
            for resid_str, _ in lp.get("req", {}).items():
                resid = int(resid_str)
                key = (lp["name"], resid)
                pk_l = Pcfg.get(key, lp["P"])
                if pk_l >= task_i["P"]:
                    Fstar.add(resid)

        Si = slack(task_i)

        # iterative loop - use compute_Psi that returns per-resid b_tilde
        while True:
            Psi_val, b_dict = compute_Psi(task_i, Pcfg)
            if Psi_val <= Si or not Fstar:
                break

            # choose best_r among Fstar maximizing b_dict[r] * c_r
            best_r = None
            best_val = -1.0
            for resid in list(Fstar):
                bval = b_dict.get(resid, 0.0)
                val = bval * float(resources[resid]["c"])
                if val > best_val:
                    best_val = val
                    best_r = resid

            if best_r is None:
                break

            # set Pk_l = Pi - 1 for all τl in llp(i)
            for lp in local_lp(task_i):
                # 只给真正请求过 best_r 的任务写 spin prio
                if str(best_r) in lp.get("req", {}):
                    key = (lp["name"], best_r)
                    newp = max(0, task_i["P"] - 1)
                    Pcfg[key] = newp

            # remove best_r from Fstar and repeat
            Fstar.discard(best_r)

# ---------------- output header file ----------------
entries = []
for (tname, resid), pr in Pcfg.items():
    t = name2task.get(tname)
    pid_hint = t.get("pid_hint", 0) if t else 0
    entries.append((tname, resid, pr, pid_hint))

with open(out_h, 'w') as f:
    f.write("/* Auto-generated by frap_table_generator.py */\n")
    f.write("#pragma once\n\n")
    f.write("#include <stdint.h>\n\n")
    f.write("struct frap_cfg_entry {\n")
    f.write("    const char *name; /* task name / label */\n")
    f.write("    int resid;\n")
    f.write("    int spin_prio; /* numeric priority */\n")
    f.write("    int pid_hint; /* optional hint used by demo */\n")
    f.write("};\n\n")
    f.write("static const struct frap_cfg_entry frap_generated_table[] = {\n")
    for (name, resid, pr, pid_hint) in entries:
        f.write('    { "%s", %d, %d, %d },\n' % (name, resid, pr, pid_hint))
    f.write("};\n\n")
    f.write("static const int frap_generated_table_len = sizeof(frap_generated_table)/sizeof(frap_generated_table[0]);\n")

print("Wrote", out_h, "with", len(entries), "entries.")
