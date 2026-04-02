#pragma once
#include "rednose/helpers/ekf.h"
extern "C" {
void live_update_4(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_9(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_10(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_12(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_35(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_32(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_13(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_14(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_update_33(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void live_H(double *in_vec, double *out_3621616873557539595);
void live_err_fun(double *nom_x, double *delta_x, double *out_4295238307701473603);
void live_inv_err_fun(double *nom_x, double *true_x, double *out_5163552264401330066);
void live_H_mod_fun(double *state, double *out_1648149972328566073);
void live_f_fun(double *state, double dt, double *out_4629883568555590600);
void live_F_fun(double *state, double dt, double *out_5707974458965497);
void live_h_4(double *state, double *unused, double *out_836921876569727748);
void live_H_4(double *state, double *unused, double *out_5690189536609713672);
void live_h_9(double *state, double *unused, double *out_557840160542147117);
void live_H_9(double *state, double *unused, double *out_8579051088889793014);
void live_h_10(double *state, double *unused, double *out_4956325736512933490);
void live_H_10(double *state, double *unused, double *out_1920785147732499546);
void live_h_12(double *state, double *unused, double *out_6464837246147257023);
void live_H_12(double *state, double *unused, double *out_6311288561657307339);
void live_h_35(double *state, double *unused, double *out_3221295756978760467);
void live_H_35(double *state, double *unused, double *out_9056851593982321048);
void live_h_32(double *state, double *unused, double *out_2110110526009169277);
void live_H_32(double *state, double *unused, double *out_3457441181115238815);
void live_h_13(double *state, double *unused, double *out_7541328692909899746);
void live_H_13(double *state, double *unused, double *out_108215062856156263);
void live_h_14(double *state, double *unused, double *out_557840160542147117);
void live_H_14(double *state, double *unused, double *out_8579051088889793014);
void live_h_33(double *state, double *unused, double *out_2640270420222513436);
void live_H_33(double *state, double *unused, double *out_6239335475088372964);
void live_predict(double *in_x, double *in_P, double *in_Q, double dt);
}