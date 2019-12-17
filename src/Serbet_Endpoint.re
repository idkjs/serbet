open Async;

module Status = {
  include Express.Response.StatusCode;
};

type request('body, 'query, 'params) = {
  req: Express.Request.t,
  requireBody: Decco.decoder('body) => Js.Promise.t('body),
  requireQuery: Decco.decoder('query) => Js.Promise.t('query),
  requireParams: Decco.decoder('params) => Js.Promise.t('params),
  requireHeader: string => Js.Promise.t(string),
};

type response =
  | BadRequest(string)
  | NotFound(string)
  | Unauthorized(string)
  | OkString(string)
  | OkJson(Js.Json.t)
  | OkBuffer(Node.Buffer.t)
  | StatusString(Status.t, string)
  | StatusJson(Status.t, Js.Json.t)
  | TemporaryRedirect(string)
  | InternalServerError
  | RespondRaw(Express.Response.t => Express.complete)
  | RespondRawAsync(Express.Response.t => promise(Express.complete));

exception HttpException(response);

let abort = res => {
  raise(HttpException(res));
};

type verb =
  | GET
  | POST
  | PUT
  | DELETE;

type guard('a) = Express.Request.t => promise('a);

module ExpressTools = {
  let queryJson = (req: Express.Request.t): Js.Json.t =>
    Obj.magic(Express.Request.query(req));

  [@bs.send] external appSet: (Express.App.t, string, 'a) => unit = "set";

  [@bs.module]
  external middlewareAsComplete:
    (Express.Middleware.t, Express.Request.t, Express.Response.t) =>
    Js.Promise.t(Express.complete) =
    "./middlewareAsComplete.js";
};

let requireHeader: string => guard(string) =
  (headerName, req) => {
    let o = req |> Express.Request.get(headerName);
    switch (o) {
    | Some(h) => async(h)
    | None => abort @@ BadRequest("Missing required header: " ++ headerName)
    };
  };

let requireBody: Decco.decoder('body) => guard('body) =
  (decoder, req) => {
    // We resolve a promise first so that if the decoding
    // fails, it will reject the promise instead of just
    // throwing and requiring a try/catch.
    let%Async _ = async();
    switch (req |> Express.Request.bodyJSON) {
    | Some(rawBodyJson) =>
      switch (decoder(rawBodyJson)) {
      | Error(e) =>
        abort(
          BadRequest(
            "Could not decode expected body: location="
            ++ e.path
            ++ ", message="
            ++ e.message,
          ),
        )
      | Ok(v) => v->async
      }
    | None => abort @@ BadRequest("Body required")
    };
  };

let requireParams: Decco.decoder('params) => guard('params) =
  (decoder, req) => {
    let paramsAsJson: Js.Json.t = Obj.magic(req |> Express.Request.params);
    // We resolve a promise first so that if the decoding
    // fails, it will reject the promise instead of just
    // throwing and requiring a try/catch.
    let%Async _ = async();
    switch (decoder(paramsAsJson)) {
    | Error(e) =>
      abort @@
      BadRequest(
        "Could not decode expected params from the URL path: location="
        ++ e.path
        ++ ", message="
        ++ e.message,
      )
    | Ok(v) => async @@ v
    };
  };

let requireQuery: Decco.decoder('query) => guard('query) =
  (decoder, req) => {
    // We resolve a promise first so that if the decoding
    // fails, it will reject the promise instead of just
    // throwing and requiring a try/catch.
    let%Async _ = async();
    switch (decoder(ExpressTools.queryJson(req))) {
    | Error(e) =>
      abort @@
      BadRequest(
        "Could not decode expected params from query string: location="
        ++ e.path
        ++ ", message="
        ++ e.message,
      )
    | Ok(v) => async @@ v
    };
  };

[@bs.module]
external jsonParsingMiddleware: Express.Middleware.t =
  "./json-parsing-middleware.js";

type endpointConfig('body_in, 'params, 'query) = {
  path: string,
  verb,
  handler: request('body_in, 'params, 'query) => Js.Promise.t(response),
};

type jsonEndpointConfig('body_in, 'params, 'query, 'body_out) = {
  path: string,
  verb,
  body_in_decode: Decco.decoder('body_in),
  body_out_encode: Decco.encoder('body_out),
  handler:
    ('body_in, request('body_in, 'params, 'query)) => Js.Promise.t('body_out),
};

type endpoint = {
  use: Express.App.t => unit,
  useOnRouter: Express.Router.t => unit,
};

let _resToExpressRes = (res, handlerRes) =>
  switch (handlerRes) {
  | BadRequest(msg) =>
    async @@
    Express.Response.(res |> status(Status.BadRequest) |> sendString(msg))
  | NotFound(msg) =>
    async @@
    Express.Response.(res |> status(Status.NotFound) |> sendString(msg))
  | Unauthorized(msg) =>
    async @@
    Express.Response.(res |> status(Status.Unauthorized) |> sendString(msg))
  | OkString(msg) =>
    async @@
    Express.Response.(
      res
      |> status(Status.Ok)
      |> setHeader("content-type", "text/plain; charset=utf-8")
      |> sendString(msg)
    )
  | OkJson(js) =>
    async @@ Express.Response.(res |> status(Status.Ok) |> sendJson(js))
  | OkBuffer(buff) =>
    async @@ Express.Response.(res |> status(Status.Ok) |> sendBuffer(buff))
  | StatusString(stat, msg) =>
    async @@
    Express.Response.(
      res
      |> status(stat)
      |> setHeader("content-type", "text/plain; charset=utf-8")
      |> sendString(msg)
    )
  | InternalServerError =>
    async @@
    Express.Response.(
      res |> sendStatus(Express.Response.StatusCode.InternalServerError)
    )
  | StatusJson(stat, js) =>
    async @@ Express.Response.(res |> status(stat) |> sendJson(js))
  | TemporaryRedirect(location) =>
    async @@
    Express.Response.(
      res
      |> setHeader("Location", location)
      |> sendStatus(StatusCode.TemporaryRedirect)
    )
  | RespondRaw(fn) => async @@ fn(res)
  | RespondRawAsync(fn) => fn(res)
  };

let defaultMiddleware = [|
  // By default we parse JSON bodies
  jsonParsingMiddleware,
|];

let endpoint =
    (~middleware=?, cfg: endpointConfig('body, 'params, 'query)): endpoint => {
  let wrappedHandler = (_next, req, res) => {
    let handleOCamlError =
      [@bs.open]
      (
        fun
        | HttpException(handlerResponse) => handlerResponse
      );
    let handleError = error => {
      switch (handleOCamlError(error)) {
      | Some(res) => res->async
      | None =>
        switch (Obj.magic(error)##stack) {
        | Some(stack) => Js.log2("Unhandled internal server error", stack)
        | None => Js.log2("Unhandled internal server error", error)
        };
        InternalServerError->async;
      };
    };

    let request = {
      req,
      requireBody: a => requireBody(a, req),
      requireQuery: a => requireQuery(a, req),
      requireParams: a => requireParams(a, req),
      requireHeader: a => requireHeader(a, req),
    };

    switch (cfg.handler(request)) {
    | exception err =>
      let%Async r = handleError(err);
      _resToExpressRes(res, r);
    | p =>
      let%Async r = p->catch(handleError);
      _resToExpressRes(res, r);
    };
  };
  let expressHandler = Express.PromiseMiddleware.from(wrappedHandler);

  let verbFunction =
    switch (cfg.verb) {
    | GET => Express.App.getWithMany
    | POST => Express.App.postWithMany
    | PUT => Express.App.putWithMany
    | DELETE => Express.App.deleteWithMany
    };

  let verbFunctionForRouter =
    switch (cfg.verb) {
    | GET => Express.Router.getWithMany
    | POST => Express.Router.postWithMany
    | PUT => Express.Router.putWithMany
    | DELETE => Express.Router.deleteWithMany
    };

  {
    use: app => {
      app->verbFunction(
        ~path=cfg.path,
        Belt.Array.concat(
          middleware->Belt.Option.getWithDefault(defaultMiddleware),
          [|expressHandler|],
        ),
      );
    },

    useOnRouter: router => {
      router->verbFunctionForRouter(
        ~path=cfg.path,
        Belt.Array.concat(
          middleware->Belt.Option.getWithDefault(defaultMiddleware),
          [|expressHandler|],
        ),
      );
    },
  };
};

let jsonEndpoint =
    (
      ~middleware=?,
      cfg: jsonEndpointConfig('body_in, 'query, 'params, 'body_out),
    )
    : endpoint => {
  endpoint(
    ~middleware?,
    {
      path: cfg.path,
      verb: cfg.verb,
      handler: req => {
        let%Async body = req.requireBody(cfg.body_in_decode);
        let%Async response = cfg.handler(body, req);
        async(OkJson(cfg.body_out_encode(response)));
      },
    },
  );
};