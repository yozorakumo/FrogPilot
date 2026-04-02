#include "car.h"

namespace {
#define DIM 9
#define EDIM 9
#define MEDIM 9
typedef void (*Hfun)(double *, double *, double *);

double mass;

void set_mass(double x){ mass = x;}

double rotational_inertia;

void set_rotational_inertia(double x){ rotational_inertia = x;}

double center_to_front;

void set_center_to_front(double x){ center_to_front = x;}

double center_to_rear;

void set_center_to_rear(double x){ center_to_rear = x;}

double stiffness_front;

void set_stiffness_front(double x){ stiffness_front = x;}

double stiffness_rear;

void set_stiffness_rear(double x){ stiffness_rear = x;}
const static double MAHA_THRESH_25 = 3.8414588206941227;
const static double MAHA_THRESH_24 = 5.991464547107981;
const static double MAHA_THRESH_30 = 3.8414588206941227;
const static double MAHA_THRESH_26 = 3.8414588206941227;
const static double MAHA_THRESH_27 = 3.8414588206941227;
const static double MAHA_THRESH_29 = 3.8414588206941227;
const static double MAHA_THRESH_28 = 3.8414588206941227;
const static double MAHA_THRESH_31 = 3.8414588206941227;

/******************************************************************************
 *                      Code generated with SymPy 1.12.1                      *
 *                                                                            *
 *              See http://www.sympy.org/ for more information.               *
 *                                                                            *
 *                         This file is part of 'ekf'                         *
 ******************************************************************************/
void err_fun(double *nom_x, double *delta_x, double *out_8735694634005437953) {
   out_8735694634005437953[0] = delta_x[0] + nom_x[0];
   out_8735694634005437953[1] = delta_x[1] + nom_x[1];
   out_8735694634005437953[2] = delta_x[2] + nom_x[2];
   out_8735694634005437953[3] = delta_x[3] + nom_x[3];
   out_8735694634005437953[4] = delta_x[4] + nom_x[4];
   out_8735694634005437953[5] = delta_x[5] + nom_x[5];
   out_8735694634005437953[6] = delta_x[6] + nom_x[6];
   out_8735694634005437953[7] = delta_x[7] + nom_x[7];
   out_8735694634005437953[8] = delta_x[8] + nom_x[8];
}
void inv_err_fun(double *nom_x, double *true_x, double *out_2508690671402078237) {
   out_2508690671402078237[0] = -nom_x[0] + true_x[0];
   out_2508690671402078237[1] = -nom_x[1] + true_x[1];
   out_2508690671402078237[2] = -nom_x[2] + true_x[2];
   out_2508690671402078237[3] = -nom_x[3] + true_x[3];
   out_2508690671402078237[4] = -nom_x[4] + true_x[4];
   out_2508690671402078237[5] = -nom_x[5] + true_x[5];
   out_2508690671402078237[6] = -nom_x[6] + true_x[6];
   out_2508690671402078237[7] = -nom_x[7] + true_x[7];
   out_2508690671402078237[8] = -nom_x[8] + true_x[8];
}
void H_mod_fun(double *state, double *out_8011515507596973417) {
   out_8011515507596973417[0] = 1.0;
   out_8011515507596973417[1] = 0;
   out_8011515507596973417[2] = 0;
   out_8011515507596973417[3] = 0;
   out_8011515507596973417[4] = 0;
   out_8011515507596973417[5] = 0;
   out_8011515507596973417[6] = 0;
   out_8011515507596973417[7] = 0;
   out_8011515507596973417[8] = 0;
   out_8011515507596973417[9] = 0;
   out_8011515507596973417[10] = 1.0;
   out_8011515507596973417[11] = 0;
   out_8011515507596973417[12] = 0;
   out_8011515507596973417[13] = 0;
   out_8011515507596973417[14] = 0;
   out_8011515507596973417[15] = 0;
   out_8011515507596973417[16] = 0;
   out_8011515507596973417[17] = 0;
   out_8011515507596973417[18] = 0;
   out_8011515507596973417[19] = 0;
   out_8011515507596973417[20] = 1.0;
   out_8011515507596973417[21] = 0;
   out_8011515507596973417[22] = 0;
   out_8011515507596973417[23] = 0;
   out_8011515507596973417[24] = 0;
   out_8011515507596973417[25] = 0;
   out_8011515507596973417[26] = 0;
   out_8011515507596973417[27] = 0;
   out_8011515507596973417[28] = 0;
   out_8011515507596973417[29] = 0;
   out_8011515507596973417[30] = 1.0;
   out_8011515507596973417[31] = 0;
   out_8011515507596973417[32] = 0;
   out_8011515507596973417[33] = 0;
   out_8011515507596973417[34] = 0;
   out_8011515507596973417[35] = 0;
   out_8011515507596973417[36] = 0;
   out_8011515507596973417[37] = 0;
   out_8011515507596973417[38] = 0;
   out_8011515507596973417[39] = 0;
   out_8011515507596973417[40] = 1.0;
   out_8011515507596973417[41] = 0;
   out_8011515507596973417[42] = 0;
   out_8011515507596973417[43] = 0;
   out_8011515507596973417[44] = 0;
   out_8011515507596973417[45] = 0;
   out_8011515507596973417[46] = 0;
   out_8011515507596973417[47] = 0;
   out_8011515507596973417[48] = 0;
   out_8011515507596973417[49] = 0;
   out_8011515507596973417[50] = 1.0;
   out_8011515507596973417[51] = 0;
   out_8011515507596973417[52] = 0;
   out_8011515507596973417[53] = 0;
   out_8011515507596973417[54] = 0;
   out_8011515507596973417[55] = 0;
   out_8011515507596973417[56] = 0;
   out_8011515507596973417[57] = 0;
   out_8011515507596973417[58] = 0;
   out_8011515507596973417[59] = 0;
   out_8011515507596973417[60] = 1.0;
   out_8011515507596973417[61] = 0;
   out_8011515507596973417[62] = 0;
   out_8011515507596973417[63] = 0;
   out_8011515507596973417[64] = 0;
   out_8011515507596973417[65] = 0;
   out_8011515507596973417[66] = 0;
   out_8011515507596973417[67] = 0;
   out_8011515507596973417[68] = 0;
   out_8011515507596973417[69] = 0;
   out_8011515507596973417[70] = 1.0;
   out_8011515507596973417[71] = 0;
   out_8011515507596973417[72] = 0;
   out_8011515507596973417[73] = 0;
   out_8011515507596973417[74] = 0;
   out_8011515507596973417[75] = 0;
   out_8011515507596973417[76] = 0;
   out_8011515507596973417[77] = 0;
   out_8011515507596973417[78] = 0;
   out_8011515507596973417[79] = 0;
   out_8011515507596973417[80] = 1.0;
}
void f_fun(double *state, double dt, double *out_4048732838979974632) {
   out_4048732838979974632[0] = state[0];
   out_4048732838979974632[1] = state[1];
   out_4048732838979974632[2] = state[2];
   out_4048732838979974632[3] = state[3];
   out_4048732838979974632[4] = state[4];
   out_4048732838979974632[5] = dt*((-state[4] + (-center_to_front*stiffness_front*state[0] + center_to_rear*stiffness_rear*state[0])/(mass*state[4]))*state[6] - 9.8000000000000007*state[8] + stiffness_front*(-state[2] - state[3] + state[7])*state[0]/(mass*state[1]) + (-stiffness_front*state[0] - stiffness_rear*state[0])*state[5]/(mass*state[4])) + state[5];
   out_4048732838979974632[6] = dt*(center_to_front*stiffness_front*(-state[2] - state[3] + state[7])*state[0]/(rotational_inertia*state[1]) + (-center_to_front*stiffness_front*state[0] + center_to_rear*stiffness_rear*state[0])*state[5]/(rotational_inertia*state[4]) + (-pow(center_to_front, 2)*stiffness_front*state[0] - pow(center_to_rear, 2)*stiffness_rear*state[0])*state[6]/(rotational_inertia*state[4])) + state[6];
   out_4048732838979974632[7] = state[7];
   out_4048732838979974632[8] = state[8];
}
void F_fun(double *state, double dt, double *out_4315169726278330996) {
   out_4315169726278330996[0] = 1;
   out_4315169726278330996[1] = 0;
   out_4315169726278330996[2] = 0;
   out_4315169726278330996[3] = 0;
   out_4315169726278330996[4] = 0;
   out_4315169726278330996[5] = 0;
   out_4315169726278330996[6] = 0;
   out_4315169726278330996[7] = 0;
   out_4315169726278330996[8] = 0;
   out_4315169726278330996[9] = 0;
   out_4315169726278330996[10] = 1;
   out_4315169726278330996[11] = 0;
   out_4315169726278330996[12] = 0;
   out_4315169726278330996[13] = 0;
   out_4315169726278330996[14] = 0;
   out_4315169726278330996[15] = 0;
   out_4315169726278330996[16] = 0;
   out_4315169726278330996[17] = 0;
   out_4315169726278330996[18] = 0;
   out_4315169726278330996[19] = 0;
   out_4315169726278330996[20] = 1;
   out_4315169726278330996[21] = 0;
   out_4315169726278330996[22] = 0;
   out_4315169726278330996[23] = 0;
   out_4315169726278330996[24] = 0;
   out_4315169726278330996[25] = 0;
   out_4315169726278330996[26] = 0;
   out_4315169726278330996[27] = 0;
   out_4315169726278330996[28] = 0;
   out_4315169726278330996[29] = 0;
   out_4315169726278330996[30] = 1;
   out_4315169726278330996[31] = 0;
   out_4315169726278330996[32] = 0;
   out_4315169726278330996[33] = 0;
   out_4315169726278330996[34] = 0;
   out_4315169726278330996[35] = 0;
   out_4315169726278330996[36] = 0;
   out_4315169726278330996[37] = 0;
   out_4315169726278330996[38] = 0;
   out_4315169726278330996[39] = 0;
   out_4315169726278330996[40] = 1;
   out_4315169726278330996[41] = 0;
   out_4315169726278330996[42] = 0;
   out_4315169726278330996[43] = 0;
   out_4315169726278330996[44] = 0;
   out_4315169726278330996[45] = dt*(stiffness_front*(-state[2] - state[3] + state[7])/(mass*state[1]) + (-stiffness_front - stiffness_rear)*state[5]/(mass*state[4]) + (-center_to_front*stiffness_front + center_to_rear*stiffness_rear)*state[6]/(mass*state[4]));
   out_4315169726278330996[46] = -dt*stiffness_front*(-state[2] - state[3] + state[7])*state[0]/(mass*pow(state[1], 2));
   out_4315169726278330996[47] = -dt*stiffness_front*state[0]/(mass*state[1]);
   out_4315169726278330996[48] = -dt*stiffness_front*state[0]/(mass*state[1]);
   out_4315169726278330996[49] = dt*((-1 - (-center_to_front*stiffness_front*state[0] + center_to_rear*stiffness_rear*state[0])/(mass*pow(state[4], 2)))*state[6] - (-stiffness_front*state[0] - stiffness_rear*state[0])*state[5]/(mass*pow(state[4], 2)));
   out_4315169726278330996[50] = dt*(-stiffness_front*state[0] - stiffness_rear*state[0])/(mass*state[4]) + 1;
   out_4315169726278330996[51] = dt*(-state[4] + (-center_to_front*stiffness_front*state[0] + center_to_rear*stiffness_rear*state[0])/(mass*state[4]));
   out_4315169726278330996[52] = dt*stiffness_front*state[0]/(mass*state[1]);
   out_4315169726278330996[53] = -9.8000000000000007*dt;
   out_4315169726278330996[54] = dt*(center_to_front*stiffness_front*(-state[2] - state[3] + state[7])/(rotational_inertia*state[1]) + (-center_to_front*stiffness_front + center_to_rear*stiffness_rear)*state[5]/(rotational_inertia*state[4]) + (-pow(center_to_front, 2)*stiffness_front - pow(center_to_rear, 2)*stiffness_rear)*state[6]/(rotational_inertia*state[4]));
   out_4315169726278330996[55] = -center_to_front*dt*stiffness_front*(-state[2] - state[3] + state[7])*state[0]/(rotational_inertia*pow(state[1], 2));
   out_4315169726278330996[56] = -center_to_front*dt*stiffness_front*state[0]/(rotational_inertia*state[1]);
   out_4315169726278330996[57] = -center_to_front*dt*stiffness_front*state[0]/(rotational_inertia*state[1]);
   out_4315169726278330996[58] = dt*(-(-center_to_front*stiffness_front*state[0] + center_to_rear*stiffness_rear*state[0])*state[5]/(rotational_inertia*pow(state[4], 2)) - (-pow(center_to_front, 2)*stiffness_front*state[0] - pow(center_to_rear, 2)*stiffness_rear*state[0])*state[6]/(rotational_inertia*pow(state[4], 2)));
   out_4315169726278330996[59] = dt*(-center_to_front*stiffness_front*state[0] + center_to_rear*stiffness_rear*state[0])/(rotational_inertia*state[4]);
   out_4315169726278330996[60] = dt*(-pow(center_to_front, 2)*stiffness_front*state[0] - pow(center_to_rear, 2)*stiffness_rear*state[0])/(rotational_inertia*state[4]) + 1;
   out_4315169726278330996[61] = center_to_front*dt*stiffness_front*state[0]/(rotational_inertia*state[1]);
   out_4315169726278330996[62] = 0;
   out_4315169726278330996[63] = 0;
   out_4315169726278330996[64] = 0;
   out_4315169726278330996[65] = 0;
   out_4315169726278330996[66] = 0;
   out_4315169726278330996[67] = 0;
   out_4315169726278330996[68] = 0;
   out_4315169726278330996[69] = 0;
   out_4315169726278330996[70] = 1;
   out_4315169726278330996[71] = 0;
   out_4315169726278330996[72] = 0;
   out_4315169726278330996[73] = 0;
   out_4315169726278330996[74] = 0;
   out_4315169726278330996[75] = 0;
   out_4315169726278330996[76] = 0;
   out_4315169726278330996[77] = 0;
   out_4315169726278330996[78] = 0;
   out_4315169726278330996[79] = 0;
   out_4315169726278330996[80] = 1;
}
void h_25(double *state, double *unused, double *out_4732531811890763561) {
   out_4732531811890763561[0] = state[6];
}
void H_25(double *state, double *unused, double *out_8546482934424742519) {
   out_8546482934424742519[0] = 0;
   out_8546482934424742519[1] = 0;
   out_8546482934424742519[2] = 0;
   out_8546482934424742519[3] = 0;
   out_8546482934424742519[4] = 0;
   out_8546482934424742519[5] = 0;
   out_8546482934424742519[6] = 1;
   out_8546482934424742519[7] = 0;
   out_8546482934424742519[8] = 0;
}
void h_24(double *state, double *unused, double *out_5501865814307034221) {
   out_5501865814307034221[0] = state[4];
   out_5501865814307034221[1] = state[5];
}
void H_24(double *state, double *unused, double *out_2345450254573581093) {
   out_2345450254573581093[0] = 0;
   out_2345450254573581093[1] = 0;
   out_2345450254573581093[2] = 0;
   out_2345450254573581093[3] = 0;
   out_2345450254573581093[4] = 1;
   out_2345450254573581093[5] = 0;
   out_2345450254573581093[6] = 0;
   out_2345450254573581093[7] = 0;
   out_2345450254573581093[8] = 0;
   out_2345450254573581093[9] = 0;
   out_2345450254573581093[10] = 0;
   out_2345450254573581093[11] = 0;
   out_2345450254573581093[12] = 0;
   out_2345450254573581093[13] = 0;
   out_2345450254573581093[14] = 1;
   out_2345450254573581093[15] = 0;
   out_2345450254573581093[16] = 0;
   out_2345450254573581093[17] = 0;
}
void h_30(double *state, double *unused, double *out_5950947004998115614) {
   out_5950947004998115614[0] = state[4];
}
void H_30(double *state, double *unused, double *out_8417143987281502449) {
   out_8417143987281502449[0] = 0;
   out_8417143987281502449[1] = 0;
   out_8417143987281502449[2] = 0;
   out_8417143987281502449[3] = 0;
   out_8417143987281502449[4] = 1;
   out_8417143987281502449[5] = 0;
   out_8417143987281502449[6] = 0;
   out_8417143987281502449[7] = 0;
   out_8417143987281502449[8] = 0;
}
void h_26(double *state, double *unused, double *out_3513827896704595237) {
   out_3513827896704595237[0] = state[7];
}
void H_26(double *state, double *unused, double *out_4804979615550686295) {
   out_4804979615550686295[0] = 0;
   out_4804979615550686295[1] = 0;
   out_4804979615550686295[2] = 0;
   out_4804979615550686295[3] = 0;
   out_4804979615550686295[4] = 0;
   out_4804979615550686295[5] = 0;
   out_4804979615550686295[6] = 0;
   out_4804979615550686295[7] = 1;
   out_4804979615550686295[8] = 0;
}
void h_27(double *state, double *unused, double *out_7336937132699461212) {
   out_7336937132699461212[0] = state[3];
}
void H_27(double *state, double *unused, double *out_6242380675481077538) {
   out_6242380675481077538[0] = 0;
   out_6242380675481077538[1] = 0;
   out_6242380675481077538[2] = 0;
   out_6242380675481077538[3] = 1;
   out_6242380675481077538[4] = 0;
   out_6242380675481077538[5] = 0;
   out_6242380675481077538[6] = 0;
   out_6242380675481077538[7] = 0;
   out_6242380675481077538[8] = 0;
}
void h_29(double *state, double *unused, double *out_7494123801050590357) {
   out_7494123801050590357[0] = state[1];
}
void H_29(double *state, double *unused, double *out_4529017948611526505) {
   out_4529017948611526505[0] = 0;
   out_4529017948611526505[1] = 1;
   out_4529017948611526505[2] = 0;
   out_4529017948611526505[3] = 0;
   out_4529017948611526505[4] = 0;
   out_4529017948611526505[5] = 0;
   out_4529017948611526505[6] = 0;
   out_4529017948611526505[7] = 0;
   out_4529017948611526505[8] = 0;
}
void h_28(double *state, double *unused, double *out_3898897454755868893) {
   out_3898897454755868893[0] = state[0];
}
void H_28(double *state, double *unused, double *out_6492648220176852756) {
   out_6492648220176852756[0] = 1;
   out_6492648220176852756[1] = 0;
   out_6492648220176852756[2] = 0;
   out_6492648220176852756[3] = 0;
   out_6492648220176852756[4] = 0;
   out_6492648220176852756[5] = 0;
   out_6492648220176852756[6] = 0;
   out_6492648220176852756[7] = 0;
   out_6492648220176852756[8] = 0;
}
void h_31(double *state, double *unused, double *out_5832939611064738870) {
   out_5832939611064738870[0] = state[8];
}
void H_31(double *state, double *unused, double *out_8577128896301702947) {
   out_8577128896301702947[0] = 0;
   out_8577128896301702947[1] = 0;
   out_8577128896301702947[2] = 0;
   out_8577128896301702947[3] = 0;
   out_8577128896301702947[4] = 0;
   out_8577128896301702947[5] = 0;
   out_8577128896301702947[6] = 0;
   out_8577128896301702947[7] = 0;
   out_8577128896301702947[8] = 1;
}
#include <eigen3/Eigen/Dense>
#include <iostream>

typedef Eigen::Matrix<double, DIM, DIM, Eigen::RowMajor> DDM;
typedef Eigen::Matrix<double, EDIM, EDIM, Eigen::RowMajor> EEM;
typedef Eigen::Matrix<double, DIM, EDIM, Eigen::RowMajor> DEM;

void predict(double *in_x, double *in_P, double *in_Q, double dt) {
  typedef Eigen::Matrix<double, MEDIM, MEDIM, Eigen::RowMajor> RRM;

  double nx[DIM] = {0};
  double in_F[EDIM*EDIM] = {0};

  // functions from sympy
  f_fun(in_x, dt, nx);
  F_fun(in_x, dt, in_F);


  EEM F(in_F);
  EEM P(in_P);
  EEM Q(in_Q);

  RRM F_main = F.topLeftCorner(MEDIM, MEDIM);
  P.topLeftCorner(MEDIM, MEDIM) = (F_main * P.topLeftCorner(MEDIM, MEDIM)) * F_main.transpose();
  P.topRightCorner(MEDIM, EDIM - MEDIM) = F_main * P.topRightCorner(MEDIM, EDIM - MEDIM);
  P.bottomLeftCorner(EDIM - MEDIM, MEDIM) = P.bottomLeftCorner(EDIM - MEDIM, MEDIM) * F_main.transpose();

  P = P + dt*Q;

  // copy out state
  memcpy(in_x, nx, DIM * sizeof(double));
  memcpy(in_P, P.data(), EDIM * EDIM * sizeof(double));
}

// note: extra_args dim only correct when null space projecting
// otherwise 1
template <int ZDIM, int EADIM, bool MAHA_TEST>
void update(double *in_x, double *in_P, Hfun h_fun, Hfun H_fun, Hfun Hea_fun, double *in_z, double *in_R, double *in_ea, double MAHA_THRESHOLD) {
  typedef Eigen::Matrix<double, ZDIM, ZDIM, Eigen::RowMajor> ZZM;
  typedef Eigen::Matrix<double, ZDIM, DIM, Eigen::RowMajor> ZDM;
  typedef Eigen::Matrix<double, Eigen::Dynamic, EDIM, Eigen::RowMajor> XEM;
  //typedef Eigen::Matrix<double, EDIM, ZDIM, Eigen::RowMajor> EZM;
  typedef Eigen::Matrix<double, Eigen::Dynamic, 1> X1M;
  typedef Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> XXM;

  double in_hx[ZDIM] = {0};
  double in_H[ZDIM * DIM] = {0};
  double in_H_mod[EDIM * DIM] = {0};
  double delta_x[EDIM] = {0};
  double x_new[DIM] = {0};


  // state x, P
  Eigen::Matrix<double, ZDIM, 1> z(in_z);
  EEM P(in_P);
  ZZM pre_R(in_R);

  // functions from sympy
  h_fun(in_x, in_ea, in_hx);
  H_fun(in_x, in_ea, in_H);
  ZDM pre_H(in_H);

  // get y (y = z - hx)
  Eigen::Matrix<double, ZDIM, 1> pre_y(in_hx); pre_y = z - pre_y;
  X1M y; XXM H; XXM R;
  if (Hea_fun){
    typedef Eigen::Matrix<double, ZDIM, EADIM, Eigen::RowMajor> ZAM;
    double in_Hea[ZDIM * EADIM] = {0};
    Hea_fun(in_x, in_ea, in_Hea);
    ZAM Hea(in_Hea);
    XXM A = Hea.transpose().fullPivLu().kernel();


    y = A.transpose() * pre_y;
    H = A.transpose() * pre_H;
    R = A.transpose() * pre_R * A;
  } else {
    y = pre_y;
    H = pre_H;
    R = pre_R;
  }
  // get modified H
  H_mod_fun(in_x, in_H_mod);
  DEM H_mod(in_H_mod);
  XEM H_err = H * H_mod;

  // Do mahalobis distance test
  if (MAHA_TEST){
    XXM a = (H_err * P * H_err.transpose() + R).inverse();
    double maha_dist = y.transpose() * a * y;
    if (maha_dist > MAHA_THRESHOLD){
      R = 1.0e16 * R;
    }
  }

  // Outlier resilient weighting
  double weight = 1;//(1.5)/(1 + y.squaredNorm()/R.sum());

  // kalman gains and I_KH
  XXM S = ((H_err * P) * H_err.transpose()) + R/weight;
  XEM KT = S.fullPivLu().solve(H_err * P.transpose());
  //EZM K = KT.transpose(); TODO: WHY DOES THIS NOT COMPILE?
  //EZM K = S.fullPivLu().solve(H_err * P.transpose()).transpose();
  //std::cout << "Here is the matrix rot:\n" << K << std::endl;
  EEM I_KH = Eigen::Matrix<double, EDIM, EDIM>::Identity() - (KT.transpose() * H_err);

  // update state by injecting dx
  Eigen::Matrix<double, EDIM, 1> dx(delta_x);
  dx  = (KT.transpose() * y);
  memcpy(delta_x, dx.data(), EDIM * sizeof(double));
  err_fun(in_x, delta_x, x_new);
  Eigen::Matrix<double, DIM, 1> x(x_new);

  // update cov
  P = ((I_KH * P) * I_KH.transpose()) + ((KT.transpose() * R) * KT);

  // copy out state
  memcpy(in_x, x.data(), DIM * sizeof(double));
  memcpy(in_P, P.data(), EDIM * EDIM * sizeof(double));
  memcpy(in_z, y.data(), y.rows() * sizeof(double));
}




}
extern "C" {

void car_update_25(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_25, H_25, NULL, in_z, in_R, in_ea, MAHA_THRESH_25);
}
void car_update_24(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<2, 3, 0>(in_x, in_P, h_24, H_24, NULL, in_z, in_R, in_ea, MAHA_THRESH_24);
}
void car_update_30(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_30, H_30, NULL, in_z, in_R, in_ea, MAHA_THRESH_30);
}
void car_update_26(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_26, H_26, NULL, in_z, in_R, in_ea, MAHA_THRESH_26);
}
void car_update_27(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_27, H_27, NULL, in_z, in_R, in_ea, MAHA_THRESH_27);
}
void car_update_29(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_29, H_29, NULL, in_z, in_R, in_ea, MAHA_THRESH_29);
}
void car_update_28(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_28, H_28, NULL, in_z, in_R, in_ea, MAHA_THRESH_28);
}
void car_update_31(double *in_x, double *in_P, double *in_z, double *in_R, double *in_ea) {
  update<1, 3, 0>(in_x, in_P, h_31, H_31, NULL, in_z, in_R, in_ea, MAHA_THRESH_31);
}
void car_err_fun(double *nom_x, double *delta_x, double *out_8735694634005437953) {
  err_fun(nom_x, delta_x, out_8735694634005437953);
}
void car_inv_err_fun(double *nom_x, double *true_x, double *out_2508690671402078237) {
  inv_err_fun(nom_x, true_x, out_2508690671402078237);
}
void car_H_mod_fun(double *state, double *out_8011515507596973417) {
  H_mod_fun(state, out_8011515507596973417);
}
void car_f_fun(double *state, double dt, double *out_4048732838979974632) {
  f_fun(state,  dt, out_4048732838979974632);
}
void car_F_fun(double *state, double dt, double *out_4315169726278330996) {
  F_fun(state,  dt, out_4315169726278330996);
}
void car_h_25(double *state, double *unused, double *out_4732531811890763561) {
  h_25(state, unused, out_4732531811890763561);
}
void car_H_25(double *state, double *unused, double *out_8546482934424742519) {
  H_25(state, unused, out_8546482934424742519);
}
void car_h_24(double *state, double *unused, double *out_5501865814307034221) {
  h_24(state, unused, out_5501865814307034221);
}
void car_H_24(double *state, double *unused, double *out_2345450254573581093) {
  H_24(state, unused, out_2345450254573581093);
}
void car_h_30(double *state, double *unused, double *out_5950947004998115614) {
  h_30(state, unused, out_5950947004998115614);
}
void car_H_30(double *state, double *unused, double *out_8417143987281502449) {
  H_30(state, unused, out_8417143987281502449);
}
void car_h_26(double *state, double *unused, double *out_3513827896704595237) {
  h_26(state, unused, out_3513827896704595237);
}
void car_H_26(double *state, double *unused, double *out_4804979615550686295) {
  H_26(state, unused, out_4804979615550686295);
}
void car_h_27(double *state, double *unused, double *out_7336937132699461212) {
  h_27(state, unused, out_7336937132699461212);
}
void car_H_27(double *state, double *unused, double *out_6242380675481077538) {
  H_27(state, unused, out_6242380675481077538);
}
void car_h_29(double *state, double *unused, double *out_7494123801050590357) {
  h_29(state, unused, out_7494123801050590357);
}
void car_H_29(double *state, double *unused, double *out_4529017948611526505) {
  H_29(state, unused, out_4529017948611526505);
}
void car_h_28(double *state, double *unused, double *out_3898897454755868893) {
  h_28(state, unused, out_3898897454755868893);
}
void car_H_28(double *state, double *unused, double *out_6492648220176852756) {
  H_28(state, unused, out_6492648220176852756);
}
void car_h_31(double *state, double *unused, double *out_5832939611064738870) {
  h_31(state, unused, out_5832939611064738870);
}
void car_H_31(double *state, double *unused, double *out_8577128896301702947) {
  H_31(state, unused, out_8577128896301702947);
}
void car_predict(double *in_x, double *in_P, double *in_Q, double dt) {
  predict(in_x, in_P, in_Q, dt);
}
void car_set_mass(double x) {
  set_mass(x);
}
void car_set_rotational_inertia(double x) {
  set_rotational_inertia(x);
}
void car_set_center_to_front(double x) {
  set_center_to_front(x);
}
void car_set_center_to_rear(double x) {
  set_center_to_rear(x);
}
void car_set_stiffness_front(double x) {
  set_stiffness_front(x);
}
void car_set_stiffness_rear(double x) {
  set_stiffness_rear(x);
}
}

const EKF car = {
  .name = "car",
  .kinds = { 25, 24, 30, 26, 27, 29, 28, 31 },
  .feature_kinds = {  },
  .f_fun = car_f_fun,
  .F_fun = car_F_fun,
  .err_fun = car_err_fun,
  .inv_err_fun = car_inv_err_fun,
  .H_mod_fun = car_H_mod_fun,
  .predict = car_predict,
  .hs = {
    { 25, car_h_25 },
    { 24, car_h_24 },
    { 30, car_h_30 },
    { 26, car_h_26 },
    { 27, car_h_27 },
    { 29, car_h_29 },
    { 28, car_h_28 },
    { 31, car_h_31 },
  },
  .Hs = {
    { 25, car_H_25 },
    { 24, car_H_24 },
    { 30, car_H_30 },
    { 26, car_H_26 },
    { 27, car_H_27 },
    { 29, car_H_29 },
    { 28, car_H_28 },
    { 31, car_H_31 },
  },
  .updates = {
    { 25, car_update_25 },
    { 24, car_update_24 },
    { 30, car_update_30 },
    { 26, car_update_26 },
    { 27, car_update_27 },
    { 29, car_update_29 },
    { 28, car_update_28 },
    { 31, car_update_31 },
  },
  .Hes = {
  },
  .sets = {
    { "mass", car_set_mass },
    { "rotational_inertia", car_set_rotational_inertia },
    { "center_to_front", car_set_center_to_front },
    { "center_to_rear", car_set_center_to_rear },
    { "stiffness_front", car_set_stiffness_front },
    { "stiffness_rear", car_set_stiffness_rear },
  },
  .extra_routines = {
  },
};

ekf_lib_init(car)
