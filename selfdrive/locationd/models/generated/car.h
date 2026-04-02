#pragma once
#include "rednose/helpers/ekf.h"
extern "C" {
void car_update_25(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_24(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_30(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_26(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_27(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_29(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_28(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_update_31(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea);
void car_err_fun(double *nom_x, double *delta_x, double *out_8735694634005437953);
void car_inv_err_fun(double *nom_x, double *true_x, double *out_2508690671402078237);
void car_H_mod_fun(double *state, double *out_8011515507596973417);
void car_f_fun(double *state, double dt, double *out_4048732838979974632);
void car_F_fun(double *state, double dt, double *out_4315169726278330996);
void car_h_25(double *state, double *unused, double *out_4732531811890763561);
void car_H_25(double *state, double *unused, double *out_8546482934424742519);
void car_h_24(double *state, double *unused, double *out_5501865814307034221);
void car_H_24(double *state, double *unused, double *out_2345450254573581093);
void car_h_30(double *state, double *unused, double *out_5950947004998115614);
void car_H_30(double *state, double *unused, double *out_8417143987281502449);
void car_h_26(double *state, double *unused, double *out_3513827896704595237);
void car_H_26(double *state, double *unused, double *out_4804979615550686295);
void car_h_27(double *state, double *unused, double *out_7336937132699461212);
void car_H_27(double *state, double *unused, double *out_6242380675481077538);
void car_h_29(double *state, double *unused, double *out_7494123801050590357);
void car_H_29(double *state, double *unused, double *out_4529017948611526505);
void car_h_28(double *state, double *unused, double *out_3898897454755868893);
void car_H_28(double *state, double *unused, double *out_6492648220176852756);
void car_h_31(double *state, double *unused, double *out_5832939611064738870);
void car_H_31(double *state, double *unused, double *out_8577128896301702947);
void car_predict(double *in_x, double *in_P, double *in_Q, double dt);
void car_set_mass(double x);
void car_set_rotational_inertia(double x);
void car_set_center_to_front(double x);
void car_set_center_to_rear(double x);
void car_set_stiffness_front(double x);
void car_set_stiffness_rear(double x);
}