$(document).ready(function() {
    console.log("HERE HERE");
    var arr = document.URL.split("/")
    console.log("/updatechatmessage/" + arr[arr.length-2] + "/" +  arr[arr.length-1]);
	setInterval(function(){
		var xhttp = new XMLHttpRequest();
		xhttp.onreadystatechange = function() {
			if(this.readyState == 4 && this.status == 200 && this.responseText){
			    console.log(this.responseText);
				document.getElementById("message-body").innerHTML = this.responseText;
			}
		};
		xhttp.open("GET", "/updatechatmessage/" + arr[arr.length-2] + "/" +  arr[arr.length-1], true);
  	    xhttp.send();
	}, 2000);
	console.log("JS IS HERE");
});