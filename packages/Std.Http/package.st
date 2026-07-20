"Std.Http package manifest. HTTP/1.1 server and client over the core
 non-blocking sockets; JSON request/response bodies through the core Json."
PackageSpec new
	name: 'Std.Http';
	version: '0.1.0';
	summary: 'HTTP/1.1 server and client over the core non-blocking sockets';
	files: #(
		'src/HttpRequest.st'
		'src/HttpConnection.st'
		'src/HttpResponse.st'
		'src/HttpRouter.st'
		'src/HttpServer.st'
		'src/HttpClient.st');
	testFiles: #(
		'tests/HttpUnitTest.st'
		'tests/HttpTest.st');
	yourself
