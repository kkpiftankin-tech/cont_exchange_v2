// F-20 dev/demo mode: auth check disabled to skip the login flow for the
// internal verification of F-12 + Sim. To re-enable, restore the previous
// implementation (isAuthenticated() + Navigate to /login) from git history.

const AuthRoute = ({ children }) => children;

export default AuthRoute;
