// Copyright 2012-2017 CNRS-UM LIRMM, CNRS-AIST JRL
//
// This file is part of RBDyn.
//
// RBDyn is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// RBDyn is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with RBDyn.  If not, see <http://www.gnu.org/licenses/>.

#include "RBDyn/TorqueFeedbackTerm.h"

#include <unsupported/Eigen/MatrixFunctions>
#include <iostream>

#include <time.h>

namespace torque_control
{

/**
 *    TorqueFeedbackTerm
 */

TorqueFeedbackTerm::TorqueFeedbackTerm(const std::vector<rbd::MultiBody>& mbs, int robotIndex,
                                       const std::shared_ptr<rbd::ForwardDynamics> fd)
  : nrDof_(mbs[robotIndex].nrDof()),
    fd_(fd),
    P_(Eigen::VectorXd::Zero(nrDof_)),
    gammaD_(Eigen::VectorXd::Zero(nrDof_))
{
  elapsed_ = {{"computeFbTerm-Gain", 0},
              {"computeFbTerm-Gain-Coriolis", 0},
              {"computeFbTerm-GammaD", 0}};
}

void TorqueFeedbackTerm::computeGammaD()
{
  // Alternative method to do
  // gammaD_ = fd_->H().inverse() * P_;

  gammaD_ = P_;
  LLT_.compute(fd_->H());
  LLT_.matrixL().solveInPlace(gammaD_);
  LLT_.matrixL().transpose().solveInPlace(gammaD_);
}

ElapsedTimeMap & TorqueFeedbackTerm::getElapsedTimes()
{
  return elapsed_;
}

/**
 *    IntegralTerm
 */

IntegralTerm::IntegralTerm(const std::vector<rbd::MultiBody> & mbs, int robotIndex,
                           const std::shared_ptr<rbd::ForwardDynamics> fd,
                           IntegralTermType intglTermType, VelocityGainType velGainType,
                           double lambda, double phiSlow, double phiFast, double fastFilterWeight,
                           double timeStep)
  : TorqueFeedbackTerm(mbs, robotIndex, fd),
    intglTermType_(intglTermType),
    velGainType_(velGainType),
    lambda_(lambda),
    coriolis_(mbs[robotIndex]),
    C_(Eigen::MatrixXd::Zero(nrDof_, nrDof_)),
    K_(Eigen::MatrixXd::Zero(nrDof_, nrDof_)),
    previousS_(Eigen::VectorXd::Zero(nrDof_)),
    fastFilteredS_(Eigen::VectorXd::Zero(nrDof_)),
    slowFilteredS_(Eigen::VectorXd::Zero(nrDof_)),
    phiSlow_(phiSlow),
    phiFast_(phiFast),
    fastFilterWeight_(fastFilterWeight),
    timeStep_(timeStep)
{
}

void IntegralTerm::computeGain(const rbd::MultiBody & mb,
			       const rbd::MultiBodyConfig & mbc_real)
{
  // std::cout << "Rafa, in IntegralTerm::computeTerm, fd_->H().rows() = " << fd_->H().rows() << std::endl;

  if (velGainType_ == MassMatrix)
  {
    K_ = lambda_ * fd_->H();
  }
  else if (velGainType_ == MassDiagonal)
  {
    K_ = lambda_ * fd_->H().diagonal().asDiagonal();
  }
  else
  {
    K_ = lambda_ * Eigen::MatrixXd::Identity(nrDof_, nrDof_);
  }

  clock_t time;

  if (intglTermType_ == PassivityBased)
  {
    time = clock();
    C_ = coriolis_.coriolis(mb, mbc_real);
    elapsed_.at("computeFbTerm-Gain-Coriolis") = (int) (clock() - time);
  }
}

void IntegralTerm::computeTerm(const rbd::MultiBody & mb,
                               const rbd::MultiBodyConfig & mbc_real,
                               const rbd::MultiBodyConfig & mbc_calc)
{
  clock_t time;

  if (intglTermType_ == Simple || intglTermType_ == PassivityBased)
  {
    time = clock();
    computeGain(mb, mbc_real);
    elapsed_.at("computeFbTerm-Gain") = (int) (clock() - time);

    Eigen::VectorXd alphaVec_ref = rbd::dofToVector(mb, mbc_calc.alpha);
    Eigen::VectorXd alphaVec_hat = rbd::dofToVector(mb, mbc_real.alpha);

    Eigen::VectorXd s = alphaVec_ref - alphaVec_hat;

    slowFilteredS_ = exp(-timeStep_*phiSlow_) * slowFilteredS_ + s- previousS_;
    fastFilteredS_ = exp(-timeStep_*phiFast_) * fastFilteredS_ + s- previousS_;

    previousS_ = s;

    Eigen::VectorXd filteredS = fastFilterWeight_*fastFilteredS_+(1-fastFilterWeight_)*slowFilteredS_;

    // std::cout << "Rafa, in computeTerm, filteredS = " << filteredS.transpose() << std::endl;

    P_ = (K_+C_) * filteredS;  // Rafa, this disabled code is just temporary... we have to use this
    // P_ = L_ * s;

    // std::cout << "Rafa, in IntegralTerm::computeTerm, P_ = " << P_.transpose() << std::endl;
    
    time = clock();
    computeGammaD();
    elapsed_.at("computeFbTerm-GammaD") = (int) (clock() - time);
  }
}

void IntegralTerm::computeTerm(const rbd::MultiBody & mb,
                               const rbd::MultiBodyConfig & mbc_real,
                               const rbd::MultiBodyConfig & mbc_calc,
                               const Eigen::VectorXd & diff_torques)
{
  computeTerm(mb, mbc_real, mbc_calc);

  std::cout << "Rafa, in IntegralTerm::computeTerm for smooth transition, diff_torques = " << diff_torques.transpose() << std::endl;

  Eigen::MatrixXd L = K_ + C_;

  if (intglTermType_ == Simple || intglTermType_ == PassivityBased)
  {

    if (fastFilterWeight_ < 1) {
      slowFilteredS_ = L.inverse() * diff_torques / (1 - fastFilterWeight_);
      fastFilteredS_.setZero();
    }
    else {
      slowFilteredS_.setZero();
      fastFilteredS_ = L.inverse() * diff_torques / fastFilterWeight_;
    }

    Eigen::VectorXd filteredS = fastFilterWeight_*fastFilteredS_+(1-fastFilterWeight_)*slowFilteredS_;
    P_ = L * filteredS;

    std::cout << "Rafa, in IntegralTerm::computeTerm for smooth transition, P_ = " << P_.transpose() << std::endl;
  }
}

/**
 *    IntegralTermAntiWindup
 */

IntegralTermAntiWindup::IntegralTermAntiWindup(const std::vector<rbd::MultiBody> & mbs, int robotIndex,
					       const std::shared_ptr<rbd::ForwardDynamics> fd,
					       IntegralTermType intglTermType,
					       VelocityGainType velGainType,
					       double lambda, double perc,
					       const Eigen::Vector3d & maxLinAcc,
					       const Eigen::Vector3d & maxAngAcc,
					       const Eigen::VectorXd & torqueL,
					       const Eigen::VectorXd & torqueU,
                                               double phiSlow, double phiFast, double fastFilterWeight,
                                               double timeStep)
  : IntegralTerm(mbs, robotIndex, fd, intglTermType, velGainType,
                lambda, phiSlow, phiFast, fastFilterWeight, timeStep),
    perc_(perc), maxLinAcc_(maxLinAcc), maxAngAcc_(maxAngAcc),
    torqueL_(torqueL), torqueU_(torqueU)
{}

void IntegralTermAntiWindup::computeTerm(const rbd::MultiBody & mb,
					 const rbd::MultiBodyConfig & mbc_real,
					 const rbd::MultiBodyConfig & mbc_calc)
{
  if (intglTermType_ == Simple || intglTermType_ == PassivityBased)
  {
    computeGain(mb, mbc_real);

    Eigen::VectorXd alphaVec_ref = rbd::dofToVector(mb, mbc_calc.alpha);
    Eigen::VectorXd alphaVec_hat = rbd::dofToVector(mb, mbc_real.alpha);

    Eigen::VectorXd s = alphaVec_ref - alphaVec_hat;

    Eigen::VectorXd torqueU_prime = torqueU_ * perc_;
    Eigen::VectorXd torqueL_prime = torqueL_ * perc_;



    for (int i = 0; i < mb.nrJoints(); i++)
    {
      if (mb.joint(i).type() == rbd::Joint::Free)
      {
        int j = mb.jointPosInDof(i);
        Eigen::Vector6d acc;
	      acc << maxAngAcc_, maxLinAcc_;
        torqueU_prime.segment<6>(j) = fd_->H().block<6, 6>(j, j).diagonal().array() * acc.array();
        torqueL_prime.segment<6>(j) = -torqueU_prime.segment<6>(j);
        break;
      }
    }

    P_  = K_*s;

    /// get the multiplier of the bound violation
    double epsilonU = (P_.array() / torqueU_prime.array()).maxCoeff();
    double epsilonL = (P_.array() / torqueL_prime.array()).maxCoeff();
    double epsilon  = std::max(std::max(epsilonU, epsilonL),1.);

    double dotprod = P_.dot(s)/epsilon;
    for (int i = 0; i < P_.size(); i++)
    {
      if (P_(i)>torqueU_prime(i))
      {
        P_(i) = torqueU_prime(i);
      }
      else if (P_(i)<torqueL_prime(i))
      {
        P_(i) = torqueL_prime(i);
      }
    }

    if (P_.dot(s)<dotprod)
    {
      assert(false && "THE ELEMENTWISE WAS A REGRESSION IN DOT PROD");
      std::cout << "THE ELEMENTWISE WAS A REGRESSION IN DOT PROD";
      std::cerr << "THE ELEMENTWISE WAS A REGRESSION IN DOT PROD";
      exit(1);
    }
    else
    {
      if ((P_.array() / torqueU_prime.array()).maxCoeff() > 1 )
      {
        std::cout << "assert ( (P_.array() / torqueU_prime.array()).maxCoeff() < 1 );" << std::endl;
        std::cerr << "assert ( (P_.array() / torqueU_prime.array()).maxCoeff() < 1 );" << std::endl;
        exit(1);
      }
      else if ( (P_.array() / torqueL_prime.array()).maxCoeff() > 1)
      {
        std::cout << "assert ( (P_.array() / torqueL_prime.array()).maxCoeff() < 1);"  << std::endl;
        std::cerr << "assert ( (P_.array() / torqueL_prime.array()).maxCoeff() < 1);"  << std::endl;
        exit(1);
      }
    }

    P_+= C_*s;

    computeGammaD();
  }
}

/**
  *    PassivityPIDTerm
  */

PassivityPIDTerm::PassivityPIDTerm(const std::vector<rbd::MultiBody> & mbs, int robotIndex,
                                   const std::shared_ptr<rbd::ForwardDynamics> fd, double timeStep,
                                   double beta, double lambda, double mu, double sigma, double cis)
  : TorqueFeedbackTerm(mbs, robotIndex, fd),
    dt_(timeStep),
    beta_(beta),
    lambda_(lambda),
    mu_(mu),
    sigma_(sigma),
    cis_(cis),
    EPrev_(Eigen::VectorXd::Zero(nrDof_)),
    coriolis_(mbs[robotIndex]),
    C_(Eigen::MatrixXd::Zero(nrDof_, nrDof_))
{
}

void PassivityPIDTerm::computeTerm(const rbd::MultiBody & mb,
                                   const rbd::MultiBodyConfig & mbc_real,
                                   const rbd::MultiBodyConfig & mbc_calc)
{
  const Eigen::MatrixXd & M = fd_->H();

  C_ = coriolis_.coriolis(mb, mbc_real);

  Eigen::MatrixXd Ka = beta_  * M;
  //Eigen::MatrixXd L  = sigma_ * M.diagonal().asDiagonal();
  Eigen::MatrixXd L  = sigma_ * M;

  Eigen::MatrixXd Kv = lambda_ * M + C_ + Ka;
  Eigen::MatrixXd Kp = mu_ * M + lambda_ * (C_ + Ka) + L;
  Eigen::MatrixXd Ki = mu_ * (C_ + Ka) + cis_ * lambda_ * L;

  Eigen::VectorXd alphaVec_ref = rbd::dofToVector(mb, mbc_calc.alpha);
  Eigen::VectorXd alphaVec_hat = rbd::dofToVector(mb, mbc_real.alpha);

  Eigen::VectorXd s = alphaVec_ref - alphaVec_hat;

  const std::vector<rbd::Joint>& joints = mb.joints();

  Eigen::VectorXd e = Eigen::VectorXd::Zero(nrDof_);

  int pos = 0;
  for (std::size_t i = 0; i < joints.size(); i++) {
    Eigen::VectorXd ei = errorParam(joints[i].type(), mbc_calc.q[i], mbc_real.q[i]);
    e.segment(pos, ei.size()) = ei;
    pos += ei.size();
  }

  Eigen::VectorXd E = EPrev_ + e * dt_;
  EPrev_ = E;

  P_ = Kv * s + Kp * e + Ki * E;

  computeGammaD();
}

Eigen::VectorXd PassivityPIDTerm::errorParam(rbd::Joint::Type type,
                                             const std::vector<double> & q_ref,
                                             const std::vector<double> & q_hat)
{
  Eigen::VectorXd e;

  switch (type) {

  case rbd::Joint::Rev:
  case rbd::Joint::Prism:

    e.resize(1);
    e[0] = (q_ref[0] - q_hat[0]);
    break;

  case rbd::Joint::Free:

    e.resize(6);

    Eigen::Quaterniond quat_ref(q_ref[0], q_ref[1], q_ref[2], q_ref[3]);
    Eigen::Quaterniond quat_hat(q_hat[0], q_hat[1], q_hat[2], q_hat[3]);

    Eigen::MatrixXd Re = quat_ref.normalized().toRotationMatrix() * quat_hat.normalized().toRotationMatrix().transpose();
    Eigen::MatrixXd Sw = Re.log();

    e[0] = Sw(2, 1);
    e[1] = Sw(0, 2);
    e[2] = Sw(1, 0);

    for (std::size_t i = 0; i < 3; i++)
      e[3 + i] = q_ref[4 + i] - q_hat[4 + i];

    break;
  }

  return e;
}

} // namespace torque_control