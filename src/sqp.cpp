#include <Eigen/Eigenvalues>
#include <cmath>
#include <iostream>
#include <solvers/bfgs.hpp>
#include <solvers/sqp.hpp>

#ifndef SOLVER_ASSERT
#define SOLVER_ASSERT(x) eigen_assert(x)
#endif

namespace sqp {

template <typename T>
SQP<T>::SQP() {
    // TODO(mi): Success is depends on QP solver settings, which is bad.
    qp_solver_.settings().warm_start = true;
    qp_solver_.settings().check_termination = 10;
    qp_solver_.settings().eps_abs = 1e-4;
    qp_solver_.settings().eps_rel = 1e-4;
    qp_solver_.settings().max_iter = 100;
    qp_solver_.settings().adaptive_rho = true;
    qp_solver_.settings().adaptive_rho_interval = 50;
    qp_solver_.settings().alpha = 1.6;
}

template <typename T>
void SQP<T>::solve(Problem& prob, const Vector& x0, const Vector& lambda0) {
    x_ = x0;
    lambda_ = lambda0;
    run_solve(prob);
}

template <typename T>
void SQP<T>::solve(Problem& prob) {
    const int nx = prob.num_var;
    const int nc = prob.num_constr;

    x_.setZero(nx);
    lambda_.setZero(nc);
    run_solve(prob);
}

template <typename T>
void SQP<T>::run_solve(Problem& prob) {
    Vector p;         // search direction
    Vector p_lambda;  // dual search direction
    Scalar alpha;     // step size

    const int nx = prob.num_var;
    const int nc = prob.num_constr;

    p.resize(nx);
    p_lambda.resize(nc);

    step_prev_.resize(nx);
    grad_L_.resize(nx);
    delta_grad_L_.resize(nx);

    Hess_.resize(nx, nx);
    grad_obj_.resize(nx);
    Jac_constr_.resize(nc, nx);
    constr_.resize(nc);
    l_.resize(nc);
    u_.resize(nc);

    info_.qp_solver_iter = 0;

    if (settings_.iteration_callback) {
        settings_.iteration_callback(*this);
    }

    int& iter = info_.iter;
    for (iter = 1; iter <= settings_.max_iter; iter++) {
        // Solve QP
        solve_qp(prob, p, p_lambda);
        p_lambda -= lambda_;

        alpha = line_search(prob, p);

        // take step
        x_ = x_ + alpha * p;
        lambda_ = lambda_ + alpha * p_lambda;

        // update step info
        step_prev_ = alpha * p;
        primal_step_norm_ = alpha * p.template lpNorm<Eigen::Infinity>();
        dual_step_norm_ = alpha * p_lambda.template lpNorm<Eigen::Infinity>();

        if (settings_.iteration_callback) {
            settings_.iteration_callback(*this);
        }

        if (termination_criteria(x_, prob)) {
            info_.status = SOLVED;
            break;
        }
    }
    if (iter > settings_.max_iter) {
        info_.status = MAX_ITER_EXCEEDED;
    }
}

template <typename Matrix>
bool is_posdef_eigen(Matrix H) {
    Eigen::EigenSolver<Matrix> eigensolver(H);
    for (int i = 0; i < eigensolver.eigenvalues().rows(); i++) {
        double v = eigensolver.eigenvalues()(i).real();
        if (v <= 0) {
            return false;
        }
    }
    return true;
}

template <typename Matrix>
bool is_posdef(Matrix H) {
    Eigen::LLT<Matrix> llt(H);
    if (llt.info() == Eigen::NumericalIssue) {
        return false;
    }
    return true;
}

template <typename T>
bool SQP<T>::termination_criteria(const Vector& x, Problem& prob) {
    if (primal_step_norm_ <= settings_.eps_prim && dual_step_norm_ <= settings_.eps_dual &&
        max_constraint_violation(x, prob) <= settings_.eps_prim) {
        return true;
    }
    return false;
}

template <typename Derived>
inline bool is_nan(const Eigen::MatrixBase<Derived>& x) {
    // return ((x.array() == x.array())).all();
    return x.array().isNaN().any();
}

template <typename T>
void SQP<T>::solve_qp(Problem& prob, Vector& step, Vector& lambda) {
    /* QP from linearized NLP:
     * minimize     0.5 x'.P.x + q'.x
     * subject to   l <= A.x + b <= u
     *
     * with:
     *   P      Hessian of Lagrangian
     *   q      objective gradient
     *   A,b    linearized constraint at current iterate
     *   l,u    constraint bounds
     *
     * transform to:
     * minimize     0.5 x'.P.x + q'.x
     * subject to   l <= A.x <= u
     *
     * Where the constraint bounds l,u set to l=u for equality constraints or
     * set to +/-INFINITY if unbounded.
     */
    prob.objective_linearized(x_, grad_obj_, obj_);
    prob.constraint_linearized(x_, Jac_constr_, constr_, l_, u_);

    delta_grad_L_ = -grad_L_;
    grad_L_ = grad_obj_ + Jac_constr_.transpose() * lambda_;

    // BFGS update
    if (info_.iter == 1) {
        Hess_.setIdentity();
    } else {
        delta_grad_L_ += grad_L_;  // delta_grad_L_ = grad_L_prev - grad_L
        BFGS_update(Hess_, step_prev_, delta_grad_L_);
    }

    if (!is_posdef(Hess_)) {
        std::cout << "Hessian not positive definite" << std::endl;
        Scalar tau = 1e-3;
        Vector v = Vector(prob.num_var);
        while (!is_posdef(Hess_)) {
            v.setConstant(tau);
            Hess_ += v.asDiagonal();
            tau *= 10;
        }
    }
    if (is_nan(Hess_)) {
        std::cout << "Hessian is NaN" << std::endl;
    }

    SOLVER_ASSERT(is_posdef(Hess_));
    SOLVER_ASSERT(!is_nan(Hess_));

    // Constraints
    // from   l <= A.x + b <= u
    // to   l-b <= A.x     <= u-b
    Vector l = l_ - constr_;
    Vector u = u_ - constr_;
    Matrix& A = Jac_constr_;
    Matrix& P = Hess_;
    Vector& q = grad_obj_;

    // solve the QP
    bool ok;
    ok = run_solve_qp(P, q, A, l, u, step, lambda);

    // if (!ok) {
    //     Hess_.setIdentity();
    //     step.setConstant(0.0);
    //     lambda.setConstant(0.0);
    // }

    // TODO:
    // B is not convex then use grad_L as step direction
    // i.e. fallback to steepest descent of Lagrangian
}

template <typename T>
bool SQP<T>::run_solve_qp(const Matrix& P, const Vector& q, const Matrix& A, const Vector& l,
                          const Vector& u, Vector& prim, Vector& dual) {
    qp_solver::QuadraticProblem<Scalar> qp_;

    qp_.P = &P;
    qp_.q = &q;
    qp_.A = &A;
    qp_.l = &l;
    qp_.u = &u;

    qp_solver_.setup(qp_);
    // qp_solver_.update_qp(qp_);
    qp_solver_.solve(qp_);

    info_.qp_solver_iter += qp_solver_.info().iter;

    if (qp_solver_.info().status == qp_solver::NUMERICAL_ISSUES) {
        printf("QPSolver NUMERICAL_ISSUES\n");
        return false;
    }
    // if (qp_solver_.info().status == qp_solver::MAX_ITER_EXCEEDED) {
    //     printf("QPSolver MAX_ITER_EXCEEDED\n");
    //     return false;
    // }

    prim = qp_solver_.primal_solution();
    dual = qp_solver_.dual_solution();

    SOLVER_ASSERT(!is_nan(prim));
    SOLVER_ASSERT(!is_nan(dual));

    return true;
}

/** Line search in direction p using l1 merit function. */
template <typename T>
typename SQP<T>::Scalar SQP<T>::line_search(Problem& prob, const Vector& p) {
    // Note: using members obj_ and grad_obj_, which are updated in solve_qp().

    Scalar mu, phi_l1, Dp_phi_l1;
    const Scalar tau = settings_.tau;  // line search step decrease, 0 < tau < settings.tau

    Scalar constr_l1 = constraint_norm(x_, prob);

    // TODO: get mu from merit function model using hessian of Lagrangian
    mu = grad_obj_.dot(p) / ((1 - settings_.rho) * constr_l1);

    phi_l1 = obj_ + mu * constr_l1;
    Dp_phi_l1 = grad_obj_.dot(p) - mu * constr_l1;

    Scalar alpha = 1.0;
    for (int i = 1; i < settings_.line_search_max_iter; i++) {
        Scalar obj_step;
        Vector x_step = x_ + alpha * p;
        prob.objective(x_step, obj_step);

        Scalar phi_l1_step = obj_step + mu * constraint_norm(x_step, prob);
        if (phi_l1_step <= phi_l1 + alpha * settings_.eta * Dp_phi_l1) {
            // accept step
            break;
        } else {
            alpha = tau * alpha;
        }
    }
    return alpha;
}

/** L1 norm of constraint violation */
template <typename T>
typename SQP<T>::Scalar SQP<T>::constraint_norm(const Vector& x, Problem& prob) {
    // Note: uses members constr_, l_ and u_ as temporary

    Scalar c_l1 = DIV_BY_ZERO_REGUL;
    prob.constraint(x, constr_, l_, u_);

    // l <= c(x) <= u
    c_l1 += (l_ - constr_).cwiseMax(0.0).sum();
    c_l1 += (constr_ - u_).cwiseMax(0.0).sum();

    return c_l1;
}

/** L_inf norm of constraint violation */
template <typename T>
typename SQP<T>::Scalar SQP<T>::max_constraint_violation(const Vector& x, Problem& prob) {
    // Note: uses members constr_, l_ and u_ as temporary

    Scalar c_max = 0;
    prob.constraint(x, constr_, l_, u_);

    // l <= c(x) <= u
    if (prob.num_constr > 0) {
        c_max = fmax(c_max, (l_ - constr_).maxCoeff());
        c_max = fmax(c_max, (constr_ - u_).maxCoeff());
    }

    return c_max;
}

template class SQP<double>;
template class SQP<float>;

}  // namespace sqp
